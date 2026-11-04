#include "msu2app/controller.hpp"
#include "diagnostic_log.hpp"
#include "msu2app/platform.hpp"
#include "msu2app/serial_guard.hpp"
#include "msu2app/flash.hpp"
#include "msu2/delta.hpp"
#include "msu2/latest.hpp"
#include "msu2/priority.hpp"
#include "msu2/session.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace msu2app {
namespace {
using Clock=std::chrono::steady_clock;
double ms(Clock::duration d){return std::chrono::duration<double,std::milli>(d).count();}
std::string stamp(){auto now=std::time(nullptr);std::tm tm{};localtime_r(&now,&tm);std::ostringstream s;s<<std::put_time(&tm,"%Y%m%dT%H%M%S");return s.str();}
void validate(const Settings& s){
    if(s.port.empty()||s.port.rfind("/dev/",0)!=0||s.port.find('\n')!=std::string::npos)throw std::invalid_argument("請選擇有效序列埠");
    if(s.fps<1||s.fps>60)throw std::invalid_argument("FPS 必須介於 1 與 60");
    if(s.hold_open_ms>60000||s.timeout_ms<1||s.timeout_ms>60000||s.chunk<6||s.chunk>65532||s.chunk%6)throw std::invalid_argument("傳輸參數超出範圍");
    msu2::parse_priority_mode(s.priority);
    const std::vector<std::string> names{"desktop","image","moving","bars","gradient","checker","solid","noise"};
    if(std::find(names.begin(),names.end(),s.source)==names.end())throw std::invalid_argument("未知畫面來源");
}
struct Source {
    Settings settings; Dependencies& deps;std::unique_ptr<ScreenCapture> capture;msu2::Image image;
    Source(Settings s,Dependencies& d):settings(std::move(s)),deps(d){
        if(deps.source)return;
        if(settings.source=="image")image=settings.prepared_image?*settings.prepared_image:load_image(settings.image_path);
        else if(settings.source=="desktop")capture=std::make_unique<ScreenCapture>();
    }
    msu2::Image get(unsigned n){
        if(deps.source)return deps.source(settings,n);
        if(capture)return capture->grab();
        if(settings.source=="image")return image;
        return msu2::make_pattern(settings.source,n);
    }
};
std::size_t crop(const std::vector<std::uint8_t>& now,const std::vector<std::uint8_t>& old,msu2::Rect r,std::vector<std::uint8_t>& out){
    if(now.size()!=25600||old.size()!=25600)throw std::logic_error("分區參考畫面無效");
    out.resize(r.width*r.height*2);std::size_t changed=0;
    for(int y=0;y<r.height;++y){const auto at=((y+r.y)*160+r.x)*2;std::copy_n(now.data()+at,r.width*2,out.data()+y*r.width*2);
        for(int x=0;x<r.width;++x)if(now[at+x*2]!=old[at+x*2]||now[at+x*2+1]!=old[at+x*2+1])++changed;}
    return changed;
}
void commit_region(std::vector<std::uint8_t>& old,const std::vector<std::uint8_t>& now,msu2::Rect r){
    for(int y=0;y<r.height;++y){auto at=((y+r.y)*160+r.x)*2;std::copy_n(now.data()+at,r.width*2,old.data()+at);}
}
}
// Threads: API callers (UI, CLI) only submit jobs, set flags and copy Snapshot.
// The worker runs loop() and every job, and is the only thread doing device I/O.
// The producer exists during stream() and touches its source, queue and counters.
//
// Snapshot.state: disconnected -> connecting -> ready (open), then per job
// streaming <-> paused, flashing or downloading. close() returns to disconnected.
// After each job loop() settles on ready or disconnected, unless the job failed:
// error then stays until a later job sets another state. Idle renewal shows
// connecting and restores the previous state on success.
struct Controller::Impl {
    enum class Action {connect,send,stream,flash,download};
    struct Job{Action action;Settings settings;unsigned page=0;std::string output_directory;};
    // mutex guards state, pending and events; wake pairs with it. log_directory is
    // fixed before the worker starts, so the worker reads it without the lock.
    mutable std::mutex mutex;std::condition_variable wake;Snapshot state;Dependencies deps;
    std::optional<Job> pending;std::atomic<bool> quitting{false},stop_requested{false},paused{false},disconnect_requested{false};
    // Worker-only once started: run, recovery_attempts, the diag_*_warned flags, lease,
    // transport, session, connection, identity, last_activity. diag itself is set
    // before the worker starts and only read afterwards, also by the producer.
    std::thread worker;std::ofstream events;unsigned run=0;std::atomic<unsigned> pause_generation{0};unsigned recovery_attempts=0;
    // Declared before session, which holds a raw pointer to it. Likewise session
    // uses transport, and lease covers transport; close() releases in that order.
    std::unique_ptr<DiagnosticLog> diag;bool diag_chunks=false,diag_fail_warned=false,diag_lost_warned=false;
    std::shared_ptr<void> lease;std::unique_ptr<msu2::Transport> transport;std::unique_ptr<msu2::Session> session;
    // last_activity is the last completed device exchange; it drives idle renewal.
    Settings connection;std::string identity;Clock::time_point last_activity=Clock::now();
    explicit Impl(std::string root,Dependencies d):deps(std::move(d)){
        if(root.empty()){const char* home=getenv("HOME");root=std::string(home?home:".")+"/Library/Logs/MSU2 Mini Studio";}
        static std::atomic<unsigned> instance{0};
        root+="/"+stamp()+"_"+std::to_string(getpid())+"_"+std::to_string(++instance);
        std::filesystem::create_directories(root);state.log_directory=root;
        events.open(root+"/events.log");if(!events)throw std::runtime_error("無法建立操作紀錄");
        if(const char* level=getenv("MSU2_DIAGNOSTICS");level&&(std::string(level)=="1"||std::string(level)=="2")){
            // Diagnostics are optional: failing to start them must not stop the App.
            try{diag=std::make_unique<DiagnosticLog>(root+"/diagnostics.csv",stamp());diag_chunks=std::string(level)=="2";
                events<<stamp()<<" 診斷紀錄已啟用（等級 "<<level<<"）：diagnostics.csv；log,end 於程式正常結束時寫入\n";}
            catch(const std::exception& e){diag.reset();events<<stamp()<<" 診斷紀錄未啟用："<<e.what()<<"\n";}
            events.flush();}
        if(!deps.lease)deps.lease=[](const std::string& p)->std::shared_ptr<void>{return std::make_shared<SerialGuard>(p);};
        if(!deps.transport)deps.transport=[](const std::string& p)->std::unique_ptr<msu2::Transport>{return std::make_unique<msu2::SerialTransport>(p);};
        worker=std::thread([this]{loop();});
    }
    // Flags read by wake's predicates must change while holding mutex. Being
    // atomic is not enough: an update between the waiter's predicate check and
    // its block would otherwise lose the notify, leaving loop() asleep forever.
    // Notify after unlocking so the woken worker can take the mutex at once.
    template<class F>void set_flags(F f){{std::lock_guard<std::mutex> lock(mutex);f();}wake.notify_all();}
    ~Impl(){set_flags([&]{quitting=true;stop_requested=true;paused=false;});if(worker.joinable())worker.join();
        // Only here is log,end written. Abnormal termination skips this entirely.
        if(diag){const auto st=diag->finish();log(diagnostics_summary(st,"結束"));}}
    static std::string diagnostics_summary(const DiagnosticLog::Status& st,const char* when){
        return std::string("診斷紀錄")+when+"：written="+std::to_string(st.written)+" lost_full="+std::to_string(st.lost_full)
            +" lost_after_failure="+std::to_string(st.lost_after_failure)+" write_failed="+(st.write_failed?"1":"0")
            +(st.write_failed?" failed_at_us="+std::to_string(st.failed_at_us):std::string())
            +(st.write_failed||st.lost_full||st.lost_after_failure?"；diagnostics.csv 不完整":"");
    }
    // Warns once per condition via events.log and Snapshot.message; never throws
    // into streaming. The writer detects failures up to ~200 ms after the event.
    void check_diagnostics(){
        if(!diag)return;
        if(!diag_fail_warned&&diag->write_failed()){diag_fail_warned=true;log("診斷紀錄寫入失敗；diagnostics.csv 自此不完整，串流不受影響");}
        if(!diag_lost_warned&&diag->lost_any()){diag_lost_warned=true;log("診斷緩衝已滿，部分診斷事件遺失；diagnostics.csv 不完整");}
    }
    // No-op unless MSU2_DIAGNOSTICS enabled the log; safe from any thread.
    void trace(const char* kind,const char* stage,const char* result,std::int64_t frame=-1,int patch=-1,std::array<std::int64_t,6> v={},std::string_view detail={}){
        if(!diag)return;msu2::TraceEvent e;e.t_ns=msu2::trace_now_ns();e.kind=kind;e.stage=stage;e.result=result;e.frame=frame;e.patch=patch;e.v=v;msu2::trace_detail(e,detail);diag->record(e);}
    template<class F>void edit(F f){std::lock_guard<std::mutex> lock(mutex);f(state);}
    void log(const std::string& message){std::lock_guard<std::mutex> lock(mutex);auto line=stamp()+" "+message;
        state.logs.push_back(line);if(state.logs.size()>200)state.logs.erase(state.logs.begin());state.message=message;
        events<<line<<'\n';events.flush();}
    bool stopped()const{return stop_requested||quitting||disconnect_requested;}
    bool delay(unsigned milliseconds){
        std::unique_lock<std::mutex> lock(mutex);wake.wait_for(lock,std::chrono::milliseconds(milliseconds),[&]{return stopped();});return !stopped();
    }
    void submit(Action action,Settings s,unsigned page=0,std::string output_directory={}){
        try{validate(s);}catch(const std::exception& e){log(e.what());return;}
        std::lock_guard<std::mutex> lock(mutex);
        if(state.busy||pending||disconnect_requested){state.message="請先停止目前工作，再執行新操作";return;}
        stop_requested=false;paused=false;state.busy=true;state.flash_progress=0;
        if(action==Action::download){state.download_progress=0;state.download_bytes=0;state.download_directory.clear();state.download_preview={};}
        pending=Job{action,std::move(s),page,std::move(output_directory)};wake.notify_all();
    }
    void close(){
        if(session)trace("connect","close","ok");
        session.reset();transport.reset();lease.reset();
        edit([](Snapshot& s){s.connected=false;s.firmware_banner=-1;s.streaming=false;s.paused=false;s.state="disconnected";});
    }
    std::string get_identity(const std::string& port){
        if(deps.source)return "offline";
        for(const auto& d:enumerate_devices())if(d.port==port)return std::to_string(d.vendor)+":"+std::to_string(d.product)+":"+d.serial;
        return {};
    }
    bool renew_idle(){
        // Conservative application threshold, not a measured firmware constant.
        // No TX is sent until a valid MSN banner is observed on the same fd.
        if(!session||Clock::now()-last_activity<std::chrono::milliseconds(4500))return false;
        std::string previous;edit([&](Snapshot& st){previous=st.state;st.state="connecting";st.connected=false;});
        const auto renewal_start=Clock::now();trace("renewal","idle","begin",-1,-1,{std::int64_t(ms(renewal_start-last_activity))});
        log("閒置後重新驗證同一連線：等待 MSN banner，再確認 MSNCN");
        try{int version=session->handshake();last_activity=Clock::now();trace("renewal","idle","ok",-1,-1,{msu2::trace_us(last_activity-renewal_start),version});
            edit([&](Snapshot& st){st.connected=true;st.firmware_banner=version;st.state=previous;++st.reconnects;});
            log("閒置會話重新驗證成功；下一張使用完整畫面");return true;
        }catch(...){trace("renewal","idle","failed",-1,-1,{msu2::trace_us(Clock::now()-renewal_start)});edit([](Snapshot& st){++st.handshake_failures;});close();throw;}
    }
    void open(const Settings& s,bool reconnect=false){
        // Clear frame/patch labels left by an earlier send before a renewal handshake.
        if(session&&connection.port==s.port&&connection.timeout_ms==s.timeout_ms&&connection.chunk==s.chunk){session->set_trace_context(-1,-1);renew_idle();return;}
        auto expected=identity;close();
        if(stopped())return;
        edit([&](Snapshot& st){st.state="connecting";st.port=s.port;});
        const auto found=get_identity(s.port);
        if(reconnect&&(!expected.empty()&&expected!=found))throw std::runtime_error("裝置識別已改變；已停止自動重連，請重新選擇裝置");
        log("檢查 cu/tty 擁有者並取得共用鎖："+s.port);trace("connect","open","begin",-1,-1,{reconnect},s.port);
        try { lease=deps.lease(s.port);transport=deps.transport(s.port);
        msu2::SessionOptions o;o.io_timeout=msu2::Milliseconds(s.timeout_ms);o.write_chunk=s.chunk;o.trace=diag.get();o.trace_chunks=diag_chunks;
        session=std::make_unique<msu2::Session>(*transport,o);
        try{
            int version=session->handshake();last_activity=Clock::now();connection=s;identity=found;trace("connect","open","ok",-1,-1,{reconnect,version});
            edit([&](Snapshot& st){st.connected=true;st.firmware_banner=version;st.state="ready";if(reconnect)++st.reconnects;});
            log("MSN 握手成功，banner="+std::to_string(version)+"；此回覆不是面板完成回執");
        }catch(...){trace("connect","open","failed",-1,-1,{reconnect});edit([](Snapshot& st){++st.handshake_failures;});throw;}
        }catch(...){close();throw;}
    }
    bool reconnect(const Settings& s){
        if(!s.auto_reconnect)return false;
        while(recovery_attempts<3&&!stopped()){
            const auto attempt=recovery_attempts++;
            log("同一裝置自動重連 "+std::to_string(attempt+1)+"/3");trace("reconnect","attempt","begin",-1,-1,{attempt+1,500ll<<attempt});if(!delay(500u<<attempt))return false;
            try{open(s,true);if(session){trace("reconnect","attempt","ok",-1,-1,{attempt+1});return true;}}catch(const std::exception& e){trace("reconnect","attempt","failed",-1,-1,{attempt+1},e.what());log(e.what());close();}
        }
        return false;
    }
    void transfer_error(const std::exception& e){
        const std::string message=e.what();
        edit([&](Snapshot& st){if(message.find("response timeout")!=std::string::npos)++st.ack_timeouts;else ++st.transport_errors;});
        trace("error",message.find("response timeout")!=std::string::npos?"ack_timeout":"transport_error","classified",-1,-1,{},message);
        log((message.find("response timeout")!=std::string::npos?"ACK 逾時：":"傳輸失敗：")+message);close();
    }
    void single(const Settings& s){
        Source source(s,deps);auto image=source.get(0);std::vector<std::uint8_t> bytes;msu2::to_rgb565(image,bytes);
        msu2::EncodedFrame frame;msu2::encode_frame(bytes,{},s.pair?msu2::Encoding::pair:msu2::Encoding::raw,frame);
        if(stopped())return;open(s);if(!session||stopped())return;
        session->set_trace_context(0,0);trace("patch","rect",s.pair?"pair":"raw",0,0,{0,0,160,80,std::int64_t(frame.payload.size()),std::int64_t(frame.pages)});
        try{auto stats=session->send(frame);last_activity=Clock::now();session->set_trace_context(-1,-1);edit([&](Snapshot& st){++st.sent;st.bytes+=stats.bytes;st.ack_ms=stats.window_ack_ms;st.write_ms=stats.payload_write_ms;st.preview=image;});}
        catch(const std::exception& e){transfer_error(e);throw;}
        // Wait even if stop/disconnect arrives; keep the user-selected send tail.
        std::this_thread::sleep_for(msu2::Milliseconds(s.hold_open_ms));
        log("單張已交付主機傳輸，送後保持 "+std::to_string(s.hold_open_ms)+" ms；連線保留");
    }
    struct Captured{unsigned id=0;msu2::Image image;Clock::time_point captured;double capture_ms=0;};
    void stream(const Settings& s){
        // Validate permissions/source before claiming the device.
        recovery_attempts=0;auto source=std::make_unique<Source>(s,deps);open(s);if(!session||stopped())return;
        const auto file=state.log_directory+"/stream_"+std::to_string(++run)+".csv";
        std::ofstream csv(file);if(!csv)throw std::runtime_error("無法建立串流 CSV");
        csv<<"frame_id,capture_ms,encode_ms,window_ack_ms,payload_write_ms,wire_bytes,dropped_total,update_kind,layout,region,outcome,transitions,trigger,region_due,region_sent,region_skipped,region_missed_queue,region_max_miss_streak,region_max_stale_ms,center_sent,left_skipped,right_skipped\n";
        edit([](Snapshot& st){st.streaming=true;st.sent=st.dropped=st.bytes=st.unchanged=st.capture_dropped=st.queue_dropped=0;st.capture_fps=st.processed_fps=st.host_fps=0;st.state="streaming";});
        log("開始串流："+s.source+" / "+std::to_string(s.fps)+" FPS / "+s.priority+"；紀錄 "+file);
        if(diag)trace("session","meta","stream_begin",-1,-1,{run,s.fps,s.pair,s.differential,s.chunk,s.timeout_ms},
            "source="+s.source+" encoding="+(s.pair?"pair":"raw")+" priority="+s.priority+" hold_open_ms="+std::to_string(s.hold_open_ms)+" auto_reconnect="+(s.auto_reconnect?"1":"0")+" csv=stream_"+std::to_string(run)+".csv");
        msu2::LatestFrame<Captured> queue;std::atomic<bool> producer_stop{false};std::atomic<std::uint64_t> capture_missed{0}, captured_total{0};
        // All times are steady_clock. start anchors scheduler time. fps_start restarts
        // on resume, and frames captured before resume_time are dropped as stale.
        // In the loop, before marks processing start; capture_ms is Source::get's
        // duration, so before-captured is queue age. CSV columns: window_ack_ms includes
        // the window write, encode_ms is the remainder after ack and payload write,
        // and dropped_total counts frame id gaps, not LatestFrame replacements.
        const auto start=Clock::now();const auto period=std::chrono::nanoseconds(1000000000ull/s.fps);
        std::thread producer([&,source=std::move(source)]()mutable{
            try{unsigned id=0;auto next=Clock::now();
                while(!stopped()&&!producer_stop){
                    if(paused){std::this_thread::sleep_for(std::chrono::milliseconds(20));next=Clock::now();continue;}
                    auto before=Clock::now();auto image=source->get(id);auto after=Clock::now();
                    ++captured_total;const auto captured_id=id;queue.put({id++,std::move(image),after,ms(after-before)});next+=period;
                    // Source follows wall time; skip elapsed capture slots, never build backlog.
                    std::int64_t missed_now=0;
                    if(next<Clock::now()){auto missing=(Clock::now()-next)/period;id+=static_cast<unsigned>(missing);capture_missed+=static_cast<std::uint64_t>(missing);next+=period*missing;missed_now=missing;}
                    if(diag)trace("capture","source","ok",captured_id,-1,{msu2::trace_us(after-before),missed_now,std::int64_t(capture_missed.load()),std::int64_t(queue.dropped())});
                    while(Clock::now()<next&&!stopped()&&!producer_stop&&!paused)std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }queue.close();
            }catch(...){queue.close(std::current_exception());}
        });
        auto join=[&]{producer_stop=true;queue.close();producer.join();
            edit([&](Snapshot& st){st.capture_dropped=capture_missed.load();st.queue_dropped=queue.dropped();});
            trace("stream","end","ok",-1,-1,{std::int64_t(capture_missed.load()),std::int64_t(queue.dropped())});
            if(diag){check_diagnostics();log(diagnostics_summary(diag->status(),"至串流結束（尚未含最後 200 ms 批次）"));}
            log("串流來源統計：capture_slots_missed="+std::to_string(capture_missed.load())+" waiting_frames_replaced="+std::to_string(queue.dropped()));};
        msu2::DeltaPlanner planner;msu2::PriorityScheduler scheduler(msu2::parse_priority_mode(s.priority),s.fps);msu2::HostCostEstimator estimator;
        // Reference rule: committed and the planner's reference hold the last image
        // fully handed over. They change only after every patch of an update returned
        // from send(), which proves the ACK prefix and host writes, not LCD completion.
        // Planning, unchanged and skipped slots commit nothing; a partitioned slot
        // merges just its rectangle. Failure, resume after pause and idle renewal clear both,
        // because the panel is then unknown, so the next update is full and unpartitioned.
        std::vector<std::uint8_t> current,committed,region;
        bool was_paused=false;unsigned observed_pause=pause_generation.load();auto resume_time=start;std::uint64_t total_sent=0, processed_total=0, capture_baseline=0;unsigned previous_id=0;bool seen_id=false;std::uint64_t gaps=0;auto fps_start=start;
        try{Captured frame;
            while(!stopped()){
                if(paused){if(!was_paused)trace("stream","pause","ok");was_paused=true;edit([](Snapshot& st){st.capture_fps=st.processed_fps=st.host_fps=0;});delay(20);continue;}
                if(was_paused||observed_pause!=pause_generation.load()){observed_pause=pause_generation.load();planner.invalidate();committed.clear();resume_time=Clock::now();was_paused=false;seen_id=false;fps_start=Clock::now();total_sent=processed_total=0;capture_baseline=captured_total.load();
                    scheduler=msu2::PriorityScheduler(msu2::parse_priority_mode(s.priority),s.fps);estimator=msu2::HostCostEstimator{};trace("stream","resume","ok");
                    edit([&](Snapshot& st){st.paused=paused.load();st.state=st.paused?"paused":"streaming";st.capture_fps=st.processed_fps=st.host_fps=0;});}
                const auto take_begin=Clock::now();
                if(!queue.take(frame))break;if(stopped())break;
                if(paused||observed_pause!=pause_generation.load()){trace("frame","pause","dropped",frame.id);continue;}
                if(frame.captured<resume_time){trace("frame","stale","dropped",frame.id);continue;}
                ++processed_total;std::int64_t id_gap=0;
                if(seen_id&&frame.id>previous_id+1){id_gap=frame.id-previous_id-1;gaps+=frame.id-previous_id-1;}previous_id=frame.id;seen_id=true;
                auto before=Clock::now();
                if(diag&&session){session->set_trace_context(frame.id,-1);}
                if(diag){
                    trace("frame","take","ok",frame.id,-1,{msu2::trace_us(before-frame.captured),msu2::trace_us(before-take_begin),id_gap,std::int64_t(gaps),std::int64_t(frame.capture_ms*1000)});}
                msu2::to_rgb565(frame.image,current);
                auto plan=planner.plan(current,s.pair?msu2::Encoding::pair:msu2::Encoding::raw,!s.differential);
                if(diag)trace("frame","plan","ok",frame.id,-1,{std::int64_t(plan.patches.size()),std::int64_t(plan.wire_bytes),std::int64_t(plan.changed_pixels),msu2::trace_us(Clock::now()-before)},msu2::update_kind_name(plan.kind));
                if(!plan.patches.empty()&&Clock::now()-last_activity>=std::chrono::milliseconds(4500)){
                    // An idle device may have restarted its session, so the old reference
                    // cannot be trusted whether or not renewal succeeds.
                    planner.invalidate();committed.clear();const auto renewal_start=Clock::now();
                    try{renew_idle();}
                    catch(const std::exception& e){
                        // renew_idle() has already counted the handshake failure and closed the
                        // session, so this bypasses transfer_error(). No slot was selected yet.
                        csv<<frame.id<<','<<frame.capture_ms<<",0,0,0,0,"<<gaps<<",failed,"<<msu2::priority_mode_name(msu2::parse_priority_mode(s.priority))<<",none,failed,"<<scheduler.transitions()<<",idle_renewal_failure,0,0,0,0,0,0,0,0,0\n";csv.flush();
                        if(!s.auto_reconnect)throw;
                        log(std::string("閒置重新驗證失敗：")+e.what());
                        if(reconnect(s)){
                            // Frames captured before the new session are stale; drop them so the
                            // first update after recovery is a newer frame sent in full.
                            resume_time=Clock::now();
                            scheduler=msu2::PriorityScheduler(msu2::parse_priority_mode(s.priority),s.fps);estimator=msu2::HostCostEstimator{};
                            edit([&](Snapshot& st){st.streaming=true;st.paused=paused.load();st.state=st.paused?"paused":"streaming";});continue;}
                        throw;
                    }
                    // Handshake wait is neither encode time nor host cost for this frame.
                    before+=Clock::now()-renewal_start;
                    plan=planner.plan(current,s.pair?msu2::Encoding::pair:msu2::Encoding::raw,true);
                    if(diag)trace("frame","replan_full","ok",frame.id,-1,{std::int64_t(plan.patches.size()),std::int64_t(plan.wire_bytes),std::int64_t(plan.changed_pixels)},msu2::update_kind_name(plan.kind));
                    scheduler=msu2::PriorityScheduler(msu2::parse_priority_mode(s.priority),s.fps);estimator=msu2::HostCostEstimator{};
                }
                auto prediction=estimator.predict(plan.wire_bytes,plan.patches.size(),frame.capture_ms+ms(Clock::now()-before));
                auto selection=scheduler.select(frame.id,planner.has_reference(),prediction.valid,prediction.ms,gaps,ms(Clock::now()-start));
                auto outcome=msu2::SlotOutcome::failed;msu2::TransferStats stats;std::string kind=msu2::update_kind_name(plan.kind);
                try{
                    if(selection.partitioned){
                        plan={};plan.changed_pixels=crop(current,committed,selection.region.rect,region);
                        if(plan.changed_pixels){msu2::Patch patch;patch.rect=selection.region.rect;msu2::encode_frame(region,patch.rect,s.pair?msu2::Encoding::pair:msu2::Encoding::raw,patch.frame);
                            plan.wire_bytes=patch.frame.wire_bytes();plan.patches.push_back(std::move(patch));}kind="priority_region";
                    }
                    auto predicted=estimator.predict(plan.wire_bytes,plan.patches.size(),frame.capture_ms+ms(Clock::now()-before));
                    if(selection.partitioned&&!plan.patches.empty()&&scheduler.skip_side(selection,predicted.valid,predicted.ms,ms(Clock::now()-start))){outcome=msu2::SlotOutcome::skipped;kind="priority_skipped";}
                    else if(plan.patches.empty()){outcome=msu2::SlotOutcome::unchanged;kind="unchanged";}
                    else{
                        auto deadline=Clock::now()+msu2::Milliseconds(s.timeout_ms);
                        for(std::size_t index=0;index<plan.patches.size();++index){const auto& patch=plan.patches[index];if(Clock::now()>=deadline)throw std::runtime_error("multi-rectangle update deadline exceeded");
                            if(diag){session->set_trace_context(frame.id,int(index));
                                trace("patch","rect",s.pair?"pair":"raw",frame.id,int(index),{patch.rect.x,patch.rect.y,patch.rect.width,patch.rect.height,std::int64_t(patch.frame.payload.size()),std::int64_t(patch.frame.pages)});}
                            auto part=session->send(patch.frame);last_activity=Clock::now();stats.bytes+=part.bytes;stats.window_ack_ms+=part.window_ack_ms;stats.payload_write_ms+=part.payload_write_ms;}
                        if(selection.partitioned){commit_region(committed,current,selection.region.rect);planner.commit(committed);}else{committed=current;planner.commit(current);}
                        outcome=msu2::SlotOutcome::sent;estimator.observe(stats.bytes,plan.patches.size(),stats.window_ack_ms,stats.payload_write_ms);++total_sent;
                        // Only a delivered update proves recovery; a handshake, unchanged or skipped
                        // slot does not. Persistent failure therefore still stops after 3 attempts.
                        recovery_attempts=0;
                    }
                    scheduler.complete(selection,outcome,frame.capture_ms+ms(Clock::now()-before),ms(Clock::now()-start));
                }catch(const std::exception& e){
                    scheduler.complete(selection,msu2::SlotOutcome::failed,ms(Clock::now()-before),ms(Clock::now()-start));
                    trace("frame","done","failed",frame.id,-1,{msu2::trace_us(Clock::now()-before),std::int64_t(stats.window_ack_ms*1000),std::int64_t(stats.payload_write_ms*1000),std::int64_t(stats.bytes),std::int64_t(plan.patches.size())},e.what());
                    csv<<frame.id<<','<<frame.capture_ms<<",0,0,0,"<<stats.bytes<<','<<gaps<<",failed,"<<msu2::priority_mode_name(selection.layout)<<','<<selection.region.name<<",failed,"<<scheduler.transitions()<<",transfer_failure,0,0,0,0,0,0,0,0,0\n";csv.flush();
                    planner.invalidate();committed.clear();transfer_error(e);
                    if(reconnect(s)){scheduler=msu2::PriorityScheduler(msu2::parse_priority_mode(s.priority),s.fps);estimator=msu2::HostCostEstimator{};edit([&](Snapshot& st){st.streaming=true;st.paused=paused.load();st.state=st.paused?"paused":"streaming";});continue;}
                    throw;
                }
                const auto done=Clock::now();
                const auto counts=selection.partitioned?scheduler.counters(selection.layout,selection.region_index):msu2::RegionCounters{};
                csv<<frame.id<<','<<frame.capture_ms<<','<<ms(done-before)-stats.window_ack_ms-stats.payload_write_ms<<','<<stats.window_ack_ms<<','<<stats.payload_write_ms<<','<<stats.bytes<<','<<gaps<<','<<kind<<','<<msu2::priority_mode_name(selection.layout)<<','<<selection.region.name<<','<<msu2::slot_outcome_name(outcome)<<','<<scheduler.transitions()<<','<<selection.trigger<<','<<counts.due<<','<<counts.sent<<','<<counts.skipped<<','<<counts.missed_queue<<','<<counts.max_consecutive_missed<<','<<counts.max_stale_ms<<','<<scheduler.counters(msu2::PriorityMode::center3,0).sent<<','<<scheduler.counters(msu2::PriorityMode::center3,1).skipped<<','<<scheduler.counters(msu2::PriorityMode::center3,2).skipped<<'\n';
                if(diag)trace("frame","done",msu2::slot_outcome_name(outcome),frame.id,-1,{msu2::trace_us(done-before),std::int64_t(stats.window_ack_ms*1000),std::int64_t(stats.payload_write_ms*1000),std::int64_t(stats.bytes),std::int64_t(plan.patches.size())},
                    kind+" "+msu2::priority_mode_name(selection.layout)+" "+selection.region.name+" "+selection.trigger);
                if(diag){if(session)session->set_trace_context(-1,-1);check_diagnostics();}
                if(frame.id%s.fps==0)csv.flush();if(!csv)throw std::runtime_error("CSV 寫入失敗，串流已停止");
                edit([&](Snapshot& st){st.preview=frame.image;st.dropped=gaps;st.capture_dropped=capture_missed.load();st.queue_dropped=queue.dropped();st.bytes+=stats.bytes;if(outcome==msu2::SlotOutcome::sent)++st.sent;if(outcome==msu2::SlotOutcome::unchanged)++st.unchanged;
                    const auto active_seconds=std::max(0.001,std::chrono::duration<double>(done-fps_start).count());
                    st.ack_ms=stats.window_ack_ms;st.write_ms=stats.payload_write_ms;st.capture_fps=(captured_total.load()-capture_baseline)/active_seconds;st.processed_fps=processed_total/active_seconds;st.host_fps=total_sent/active_seconds;st.active_layout=msu2::priority_mode_name(selection.layout);st.region=selection.region.name;});
            }
        }catch(...){join();throw;}join();csv.flush();
        std::this_thread::sleep_for(msu2::Milliseconds(s.hold_open_ms));log("串流已停止，連線保留；CSV 已寫入");
    }
    void flash(const Settings& s,unsigned page){
        if(s.source!="image")throw std::invalid_argument("素材燒錄需先選擇圖片來源");
        Source source(s,deps);auto image=source.get(0);std::vector<std::uint8_t> pixels;msu2::to_rgb565(image,pixels);auto plan=make_flash_plan(std::move(pixels),page);
        if(stopped())return;open(s);if(!session||stopped())return;
        edit([&](Snapshot& st){st.state="flashing";st.preview=image;});
        const auto base=state.log_directory+"/flash_"+std::to_string(++run)+"_page"+std::to_string(page);
        std::ofstream manifest(base+".txt");if(!manifest)throw std::runtime_error("無法建立燒錄紀錄");
        manifest<<"port="<<s.port<<"\nstart_page="<<page<<"\nend_page="<<plan.end_page()<<"\nbytes="<<plan.rgb565.size()<<"\nsource="<<s.image_path<<"\n";manifest.flush();
        // Back up entire physical 4 KiB sectors touched, then verify they remain
        // identical outside the planned pages. This retains neighboring evidence.
        const auto backup_start=(page/16)*4096;const auto backup_end=((page+100+15)/16)*4096;
        try{
            log("燒錄前備份涵蓋範圍（含相鄰 sector 資料）；此階段可停止");
            auto backup=read_flash_bytes(*transport,backup_start,backup_end-backup_start,msu2::Milliseconds(s.timeout_ms),[&]{return stopped();});
            std::ofstream out(base+"_backup.bin",std::ios::binary);out.write(reinterpret_cast<const char*>(backup.data()),backup.size());out.flush();if(!out)throw std::runtime_error("备份寫入失敗，未開始擦除");out.close();
            std::ofstream intended(base+"_intended.rgb565",std::ios::binary);intended.write(reinterpret_cast<const char*>(plan.rgb565.data()),plan.rgb565.size());intended.flush();if(!intended)throw std::runtime_error("素材紀錄寫入失敗，未開始擦除");
            manifest<<"backup_start_byte="<<backup_start<<"\nbackup_bytes="<<backup.size()<<"\nbackup_saved=true\n";manifest.flush();if(!manifest)throw std::runtime_error("燒錄紀錄失敗，未開始擦除");
            FlashOptions o;o.io_timeout=msu2::Milliseconds(s.timeout_ms);
            const auto result=execute_flash(*transport,plan,o,[&](const FlashProgress& p){
                manifest<<"phase="<<flash_phase_name(p.phase)<<" completed="<<p.completed<<" total="<<p.total<<" page="<<p.page<<'\n';manifest.flush();if(!manifest)throw std::runtime_error("燒錄進度紀錄失敗；結果可能為部分寫入");
                edit([&](Snapshot& st){st.flash_progress=p.total?double(p.completed)/p.total:0;st.message=std::string("素材燒錄：")+flash_phase_name(p.phase)+" "+std::to_string(p.completed)+"/"+std::to_string(p.total);});
            },[&]{return stopped();});
            manifest<<"pages_written="<<result.pages_written<<"\nbytes_verified="<<result.bytes_verified<<"\ncancelled="<<result.cancelled<<'\n';
            if(result.cancelled){log("燒錄已取消；可能已有部分頁變更，備份與紀錄已保存");close();}
            else{
                auto before_len=page*256-backup_start;auto after_addr=(page+100)*256;
                auto before=before_len?read_flash_bytes(*transport,backup_start,before_len,msu2::Milliseconds(s.timeout_ms)):std::vector<std::uint8_t>{};
                auto after=backup_end>after_addr?read_flash_bytes(*transport,after_addr,backup_end-after_addr,msu2::Milliseconds(s.timeout_ms)):std::vector<std::uint8_t>{};
                bool neighbors=std::equal(before.begin(),before.end(),backup.begin())&&std::equal(after.begin(),after.end(),backup.begin()+after_addr-backup_start);
                manifest<<"neighbors_verified="<<neighbors<<'\n';
                if(!neighbors)throw std::runtime_error("素材已寫入，但相鄰頁讀回與備份不符；已停止，請保留備份");
                last_activity=Clock::now();edit([](Snapshot& st){st.flash_progress=1;});log("素材燒錄完成；25,600 bytes 及涵蓋 sector 相鄰資料讀回比對通過");
            }
        }catch(const std::exception& e){manifest<<"outcome=failed_or_cancelled\nerror="<<e.what()<<'\n';manifest.flush();close();throw;}
    }
    void download(const Settings& s,unsigned page,const std::string& parent){
        namespace fs=std::filesystem;
        if(page>flash_total_pages-flash_image_pages)throw std::invalid_argument("下載起始頁必須介於 0 與 3996");
        if(parent.empty()||!fs::is_directory(parent))throw std::invalid_argument("請選擇有效的下載資料夾");
        const auto folder=fs::path(parent)/("MSU2_page"+std::to_string(page)+"-"+std::to_string(page+99)+"_"+stamp()+"_"+std::to_string(getpid())+"_"+std::to_string(++run));
        if(!fs::create_directory(folder))throw std::runtime_error("下載資料夾已存在；請重新選擇儲存位置");
        std::ofstream manifest(folder/"manifest.txt");
        if(!manifest)throw std::runtime_error("無法建立下載紀錄");
        manifest<<"port="<<s.port<<"\nstart_page="<<page<<"\nend_page="<<page+99
                <<"\nstart_byte="<<page*256<<"\nbyte_count="<<flash_image_bytes
                <<"\nwidth=160\nheight=80\nformat=RGB565 big-endian, top-down\noperation=read_only\nstatus=reading\n";
        manifest.flush();if(!manifest)throw std::runtime_error("無法儲存下載紀錄");
        auto cleanup=[&]{std::error_code ignored;
            for(const auto* name:{"image.rgb565.part","image.png.part","image.rgb565","image.png"})fs::remove(folder/name,ignored);};
        try{
            if(stopped())throw FlashReadCancelled();
            open(s);if(!session||stopped())throw FlashReadCancelled();
            edit([](Snapshot& st){st.state="downloading";});
            log("讀取裝置圖片：第 "+std::to_string(page)+"–"+std::to_string(page+99)+" 頁，25,600 bytes");
            std::vector<std::uint8_t> bytes;
            try{
                bytes=read_flash_bytes(*transport,page*256,flash_image_bytes,msu2::Milliseconds(s.timeout_ms),
                    [&]{return stopped();},[&](std::size_t done,std::size_t total){
                        last_activity=Clock::now();
                        edit([&](Snapshot& st){st.download_bytes=done;st.download_progress=double(done)/total;
                            st.message="讀取圖片："+std::to_string(done)+" / "+std::to_string(total)+" bytes";});
                    });
            }catch(const FlashReadCancelled&){throw;}
            catch(const std::exception& e){transfer_error(e);throw;}
            if(stopped())throw FlashReadCancelled();
            auto image=decode_flash_image(bytes);
            {std::ofstream raw(folder/"image.rgb565.part",std::ios::binary);
                raw.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());raw.flush();
                if(!raw)throw std::runtime_error("RGB565 下載檔案儲存失敗");}
            save_image_png((folder/"image.png.part").string(),image);
            if(stopped())throw FlashReadCancelled();
            fs::rename(folder/"image.rgb565.part",folder/"image.rgb565");
            fs::rename(folder/"image.png.part",folder/"image.png");
            manifest<<"bytes_received="<<bytes.size()<<"\nraw_file=image.rgb565\npng_file=image.png\nstatus=complete\n";
            manifest.flush();if(!manifest)throw std::runtime_error("下載完成紀錄儲存失敗");
            last_activity=Clock::now();
            edit([&](Snapshot& st){st.download_progress=1;st.download_directory=folder.string();st.download_preview=std::move(image);++st.download_generation;});
            log("圖片下載完成："+folder.string()+"（PNG、RGB565、頁面資訊）");
        }catch(const FlashReadCancelled&){
            cleanup();manifest<<"status=cancelled\n";manifest.flush();close();log("圖片讀取已取消；尚未產生完整圖片");
        }catch(const std::exception& e){
            cleanup();manifest<<"status=failed\nerror="<<e.what()<<'\n';manifest.flush();close();throw;
        }
    }
    void loop(){
        while(!quitting){std::optional<Job> job;
            {std::unique_lock<std::mutex> lock(mutex);wake.wait(lock,[&]{return quitting||pending||disconnect_requested;});if(quitting)break;job=std::move(pending);pending.reset();}
            if(job){
                try{switch(job->action){case Action::connect:open(job->settings);break;case Action::send:single(job->settings);break;case Action::stream:stream(job->settings);break;case Action::flash:flash(job->settings,job->page);break;case Action::download:download(job->settings,job->page,job->output_directory);break;}}
                catch(const std::exception& e){log(e.what());edit([](Snapshot& st){st.state="error";});}
                edit([&](Snapshot& st){st.busy=false;st.streaming=false;st.paused=false;st.capture_fps=st.processed_fps=st.host_fps=0;if(st.state!="error")st.state=st.connected?"ready":"disconnected";});
            }
            check_diagnostics();
            if(disconnect_requested){close();disconnect_requested=false;log("已中斷連線並釋放序列埠");}
        }
        close();
    }
};
Controller::Controller(std::string root):Controller(std::move(root),{}){}
Controller::Controller(std::string root,Dependencies d):impl_(std::make_unique<Impl>(std::move(root),std::move(d))){}
Controller::~Controller()=default;
void Controller::connect(Settings s){impl_->submit(Impl::Action::connect,std::move(s));}
void Controller::send_image(Settings s){impl_->submit(Impl::Action::send,std::move(s));}
void Controller::start_stream(Settings s){impl_->submit(Impl::Action::stream,std::move(s));}
void Controller::flash_image(Settings s,unsigned p){impl_->submit(Impl::Action::flash,std::move(s),p);}
void Controller::download_image(Settings s,unsigned p,std::string directory){impl_->submit(Impl::Action::download,std::move(s),p,std::move(directory));}
void Controller::disconnect(){impl_->set_flags([&]{impl_->stop_requested=true;impl_->disconnect_requested=true;impl_->paused=false;});}
void Controller::pause(bool value){
    {std::lock_guard<std::mutex> lock(impl_->mutex);if(!impl_->state.streaming)return;
     if(impl_->paused.exchange(value)!=value)++impl_->pause_generation;
     impl_->state.paused=value;impl_->state.state=value?"paused":"streaming";
     if(value)impl_->state.capture_fps=impl_->state.processed_fps=impl_->state.host_fps=0;}impl_->wake.notify_all();
}
void Controller::stop(){impl_->set_flags([&]{impl_->stop_requested=true;impl_->paused=false;});}
Snapshot Controller::snapshot()const{std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->state;}
}
