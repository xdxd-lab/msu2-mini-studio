#include "msu2/capture.hpp"
#include "msu2/delta.hpp"
#include "msu2/image.hpp"
#include "msu2/latest.hpp"
#include "msu2/protocol.hpp"
#include "msu2/priority.hpp"
#include "msu2/session.hpp"
#include "msu2/transport.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock=std::chrono::steady_clock;
static_assert(std::atomic<bool>::is_always_lock_free,"signal flag requires lock-free atomics");
std::atomic<bool> interrupted{false};
void interrupt_handler(int) {interrupted.store(true,std::memory_order_relaxed);}
double ms(Clock::duration d) {return std::chrono::duration<double,std::milli>(d).count();}
struct Args {
    std::string command;
    std::map<std::string,std::string> values;
    bool has(const std::string& k)const {return values.count(k)!=0;}
    std::string get(const std::string& k,const std::string& fallback="")const {
        auto p=values.find(k);return p==values.end()?fallback:p->second;
    }
    unsigned num(const std::string& k,unsigned fallback,unsigned lo,unsigned hi)const {
        if(!has(k))return fallback;
        const auto v=get(k);
        if(v.empty() || !std::all_of(v.begin(),v.end(),[](unsigned char c){return c>='0'&&c<='9';}))
            throw std::invalid_argument(k+" requires an unsigned integer");
        unsigned long long n=0;
        for(char c:v) {n=n*10+unsigned(c-'0');if(n>hi)throw std::invalid_argument(k+" is out of range");}
        if(n<lo)throw std::invalid_argument(k+" is out of range");
        return unsigned(n);
    }
    std::string required(const std::string& k)const {
        if(!has(k)||get(k).empty())throw std::invalid_argument("missing "+k);
        return get(k);
    }
};
Args parse(int argc,char** argv) {
    Args a;a.command=argc>1?argv[1]:"help";
    static const std::map<std::string,std::set<std::string>> allowed{
        {"ports",{}}, {"probe",{"--port","--timeout-ms"}},
        {"encode",{"--input","--pattern","--mode","--output"}},
        {"preview",{"--input","--pattern","--desktop","--output"}},
        {"send",{"--input","--pattern","--desktop","--mode","--port","--timeout-ms","--chunk","--ack","--orientation","--hold-open-ms"}},
        {"stream",{"--input","--pattern","--desktop","--mode","--port","--timeout-ms","--chunk","--ack","--orientation","--frames","--fps","--csv","--update","--keyframe-ms","--priority-mode"}},
        {"bench",{"--frames","--csv"}},
        {"diff-bench",{"--frames","--csv"}}
    };
    if(a.command=="--help"||a.command=="-h"||a.command=="help") {a.command="help";return a;}
    if(!allowed.count(a.command))throw std::invalid_argument("unknown command: "+a.command);
    for(int i=2;i<argc;++i) {
        std::string k=argv[i];
        if(k=="--help") {a.command="help";return a;}
        if(!allowed.at(a.command).count(k))throw std::invalid_argument("unknown option for "+a.command+": "+k);
        if(a.has(k))throw std::invalid_argument("duplicate option: "+k);
        if(k=="--desktop") a.values[k]="true";
        else {
            if(i+1==argc)throw std::invalid_argument("missing value for "+k);
            a.values[k]=argv[++i];
        }
    }
    return a;
}
void help() {
    std::cout<<R"(MSU2 MINI Linux candidate v0.3.1 (160x80; existing firmware)

  msu2 ports
  msu2 probe --port /dev/ttyACM0
  msu2 preview --pattern bars --output bars.ppm
  msu2 preview --desktop --output desktop.ppm        (Windows only)
  msu2 encode --pattern bars --mode pair --output bars.bin
  msu2 send --port /dev/ttyACM0 --pattern bars
  msu2 send --port /dev/ttyACM0 --input picture.ppm
  msu2 stream --port /dev/ttyACM0 --pattern moving --update diff --fps 60 --frames 600 --csv diff60.csv
  msu2 stream --port /dev/ttyACM0 --pattern moving --frames 120 --fps 30
  msu2 stream --port /dev/ttyACM0 --pattern moving --priority-mode bands4 --fps 45 --frames 450 --csv bands.csv
  msu2 bench --frames 2000 --csv bench.csv
  msu2 diff-bench --frames 300 --csv diff_bench.csv

Sources: --pattern bars|solid|checker|gradient|noise|moving OR --input P6/P3.ppm.
Images are resized/cropped to 160x80. Default: bars; stream default: moving.
Transport options: --timeout-ms 2000 --chunk 60 --ack prefix|echo
  prefix is the documented legacy 02 03 response check, NOT frame completion.
  echo requires a complete echoed LCD command; experimental firmware-dependent.
  chunk must be a multiple of 6; use 60 to compare legacy small writes.
  --orientation 0|1 optionally sends a firmware orientation state (angle unverified).
Send: --hold-open-ms 13 (default; range 0..60000) keeps the serial port open after
  payload writes, without further serial I/O. This is not LCD completion ACK.
--frames in stream counts captured frames; an overrun replaces the waiting frame.
Stream: --update full|diff (default full, for baseline compatibility).
  diff compares RGB565 pixels with the last fully sent source frame, then selects
  merged rectangles or a full frame based on encoded size and per-window cost.
  Unchanged frames send nothing; they are counted separately, NOT as sent FPS.
  --keyframe-ms 1000 forces periodic full frames in diff mode; 0 disables them.
  Range: 0..60000. First frame always full. Diff mode requires retained LCD pixels.
  CSV rows include unchanged processed frames; update_kind identifies the action.
  --priority-mode off|bands4|center3|halves2 (default off) selects adaptive
  spatial scheduling when host transfer load is predicted or observed high.
  bands4: four 160x20 rows, top to bottom. center3: center 80x80, then
  left and right 40x80; overloaded side slots may be skipped. halves2:
  left then right 80x80. First frame is full; split-mode slots use source
  frame IDs and commit only successfully sent regions. Returning to normal
  full/diff planning requires sustained lower load. CSV distinguishes source
  queue replacements, intentional side skips, unchanged regions and failures.
diff-bench: offline sequences, first frame full, no periodic keyframes or device I/O.
CSV outputs refuse existing paths; choose a new filename for each run.
Ctrl+C stops stream; an in-flight frame completes or reaches its I/O timeout.
Ports are explicit. Stop other screen apps first. Replug if no MSN banner appears.
No Flash/MCU firmware writes. No real-hardware performance claim. .bin from encode
is an OFFLINE COMMAND STREAM, not a Flash image. OS write completion is not LCD FPS.
)";
}
void check_csv_path(const Args& a) {
    if(a.has("--csv") && std::filesystem::exists(a.required("--csv")))
        throw std::invalid_argument("CSV path already exists; choose a new filename: "+a.get("--csv"));
}
msu2::Encoding encoding(const Args& a) {
    const auto s=a.get("--mode","pair");
    if(s=="raw")return msu2::Encoding::raw;
    if(s=="pair")return msu2::Encoding::pair;
    throw std::invalid_argument("--mode must be raw or pair");
}
msu2::SessionOptions session_options(const Args& a) {
    msu2::SessionOptions o;
    const auto timeout=a.num("--timeout-ms",2000,1,60000);
    o.io_timeout=msu2::Milliseconds(timeout);
    o.handshake_timeout=msu2::Milliseconds(a.has("--timeout-ms")?timeout:5000);
    o.write_chunk=a.num("--chunk",60,6,65532);
    if(o.write_chunk%6)throw std::invalid_argument("--chunk must be divisible by 6");
    const auto ack=a.get("--ack","prefix");
    if(ack=="prefix")o.ack_mode=msu2::AckMode::prefix;
    else if(ack=="echo")o.ack_mode=msu2::AckMode::echo;
    else throw std::invalid_argument("--ack must be prefix or echo");
    if(a.has("--orientation"))a.num("--orientation",0,0,1);
    return o;
}
struct Source {
    std::string pattern;
    msu2::Image still;
    std::unique_ptr<msu2::DesktopCapture> desktop;
    explicit Source(const Args& a) {
        if(unsigned(a.has("--pattern"))+unsigned(a.has("--input"))+unsigned(a.has("--desktop"))>1)
            throw std::invalid_argument("choose only one of --pattern, --input, --desktop");
        if(a.has("--desktop"))desktop=std::make_unique<msu2::DesktopCapture>();
        else if(a.has("--input"))still=msu2::resize_crop(msu2::read_ppm(a.required("--input")),160,80);
        else {pattern=a.get("--pattern",a.command=="stream"?"moving":"bars");(void)msu2::make_pattern(pattern);}
    }
    msu2::Image get(unsigned tick=0) {
        if(desktop)return desktop->grab();
        if(!pattern.empty())return msu2::make_pattern(pattern,tick);
        return still;
    }
};
void save_commands(const std::string& path,const msu2::EncodedFrame& frame) {
    std::ofstream out(path,std::ios::binary);
    if(!out)throw std::runtime_error("cannot create command file: "+path);
    out.write(reinterpret_cast<const char*>(frame.window.data()),frame.window.size());
    out.write(reinterpret_cast<const char*>(frame.payload.data()),static_cast<std::streamsize>(frame.payload.size()));
    if(!out)throw std::runtime_error("command file write failed");
}
void connection(msu2::Session& s,const Args& a) {
    int version=s.handshake();
    std::cout<<"MSN handshake verified; firmware_banner="<<version
             <<"; configured_model=MINI_160x80 (dimensions not queried)\n";
    if(a.has("--orientation"))s.set_orientation(a.num("--orientation",0,0,1));
}
void print_stats(const msu2::TransferStats& st) {
    std::cout<<"host_write_bytes="<<st.bytes<<" write_calls="<<st.write_calls
             <<" window_ack_ms="<<st.window_ack_ms<<" payload_write_ms="<<st.payload_write_ms
             <<"; LCD completion not measured\n";
}
struct Captured {
    unsigned id=0;
    msu2::Image image;
    Clock::time_point captured_at;
    double capture_ms=0;
};
std::size_t crop_and_count(const std::vector<std::uint8_t>& current,
                           const std::vector<std::uint8_t>& reference,
                           msu2::Rect rect,std::vector<std::uint8_t>& cropped) {
    msu2::validate_rect(rect);
    if(current.size()!=std::size_t(msu2::screen_width*msu2::screen_height*2) ||
       reference.size()!=current.size())
        throw std::invalid_argument("priority region requires a complete committed canvas");
    const auto row_bytes=std::size_t(rect.width)*2;
    cropped.resize(row_bytes*std::size_t(rect.height));
    std::size_t changed=0;
    for(int y=0;y<rect.height;++y) {
        const auto source=(std::size_t(rect.y+y)*msu2::screen_width+std::size_t(rect.x))*2;
        const auto dest=std::size_t(y)*row_bytes;
        std::copy_n(current.data()+source,row_bytes,cropped.data()+dest);
        for(int x=0;x<rect.width;++x) {
            const auto p=source+std::size_t(x)*2;
            if(current[p]!=reference[p] || current[p+1]!=reference[p+1]) ++changed;
        }
    }
    return changed;
}
void commit_region(std::vector<std::uint8_t>& reference,
                   const std::vector<std::uint8_t>& current,msu2::Rect rect) {
    const auto row_bytes=std::size_t(rect.width)*2;
    for(int y=0;y<rect.height;++y) {
        const auto at=(std::size_t(rect.y+y)*msu2::screen_width+std::size_t(rect.x))*2;
        std::copy_n(current.data()+at,row_bytes,reference.data()+at);
    }
}
void stream(const Args& a) {
    auto mode=encoding(a);
    auto options=session_options(a);
    auto count=a.num("--frames",300,1,10000000);
    auto fps=a.num("--fps",30,1,240);
    auto port=a.required("--port");
    auto update=a.get("--update","full");
    if(update!="full" && update!="diff")throw std::invalid_argument("--update must be full or diff");
    const bool differential=update=="diff";
    if(a.has("--keyframe-ms") && !differential)
        throw std::invalid_argument("--keyframe-ms requires --update diff");
    auto keyframe_ms=a.num("--keyframe-ms",1000,0,60000);
    if(!a.has("--desktop")){Source validate_source(a);}
    else if(a.has("--input")||a.has("--pattern"))throw std::invalid_argument("--desktop cannot be combined with another source");
#ifndef _WIN32
    if(a.has("--desktop"))throw std::runtime_error("Desktop capture is supported only on Windows in this prototype");
#endif
    check_csv_path(a);
    msu2::SerialTransport transport(port);
    msu2::Session session(transport,options);
    connection(session,a);
    std::ofstream csv;
    if(a.has("--csv")) {
        // Create after the handshake, so a failed new connection cannot leave
        // a misleading empty run. Existing outputs were rejected above.
        csv.open(a.required("--csv"));if(!csv)throw std::runtime_error("cannot create stream CSV");
        csv<<"frame_id,capture_ms,convert_ms,encode_ms,window_ack_ms,payload_write_ms,host_latency_ms,wire_bytes,write_calls,dropped_total,changed_pixels,transmitted_pixels,rectangles,update_kind,unchanged_total\n";
    }
    std::cout<<"update="<<update<<" encoding="<<(mode==msu2::Encoding::raw?"raw":"pair")
             <<" target_fps="<<fps<<" keyframe_ms="<<(differential?keyframe_ms:0)<<'\n';
    msu2::LatestFrame<Captured> queue;
    std::atomic<bool> stop{false};
    std::atomic<unsigned> produced{0};
    const auto period=std::chrono::nanoseconds(1000000000ull/fps);
    auto start=Clock::now();
    std::thread producer([&]{
        try {
            Source source(a);
            auto next=Clock::now();
            for(unsigned i=0;i<count && !stop.load() && !interrupted;++i) {
                auto before=Clock::now();auto image=source.get(i);auto after=Clock::now();
                queue.put(Captured{i,std::move(image),after,ms(after-before)});++produced;
                next+=period;
                if(next<Clock::now())next=Clock::now();
                if(i+1<count)std::this_thread::sleep_until(next);
            }
            queue.close();
        } catch(...) {queue.close(std::current_exception());}
    });
    unsigned sent=0,processed=0,unchanged=0,full_frames=0,partial_frames=0;
    std::size_t wire=0,rectangles_sent=0;
    msu2::DeltaPlanner planner;
    std::vector<std::uint8_t> rgb565;
    auto last_report=start,last_full=start;
    try {
        Captured frame;
        while(!interrupted && queue.take(frame)) {
            const auto before=Clock::now();msu2::to_rgb565(frame.image,rgb565);const auto converted=Clock::now();
            const bool keyframe=differential && keyframe_ms && converted-last_full>=msu2::Milliseconds(keyframe_ms);
            auto plan=planner.plan(rgb565,mode,!differential || keyframe);
            const auto after=Clock::now();
            msu2::TransferStats st{};
            try {
                // Complete all rectangles of this source frame before taking
                // another. A partially transmitted update is never committed.
                const auto update_deadline=after+options.io_timeout;
                for(const auto& patch:plan.patches) {
                    if(Clock::now()>=update_deadline)
                        throw std::runtime_error("multi-rectangle update deadline exceeded");
                    auto part=session.send(patch.frame);
                    st.bytes+=part.bytes;st.write_calls+=part.write_calls;
                    st.window_ack_ms+=part.window_ack_ms;st.payload_write_ms+=part.payload_write_ms;
                }
            } catch(...) {planner.invalidate();throw;}
            auto done=Clock::now();++processed;
            if(plan.patches.empty()) ++unchanged;
            else {
                planner.commit(rgb565);++sent;wire+=st.bytes;rectangles_sent+=plan.patches.size();
                if(plan.kind==msu2::UpdateKind::full) {++full_frames;last_full=done;}
                else ++partial_frames;
            }
            if(csv.is_open()) {
                csv<<frame.id<<','<<frame.capture_ms<<','<<ms(converted-before)<<','<<ms(after-converted)<<','
                   <<st.window_ack_ms<<','<<st.payload_write_ms<<','<<ms(done-frame.captured_at)<<','
                   <<st.bytes<<','<<st.write_calls<<','<<queue.dropped()<<','<<plan.changed_pixels<<','
                   <<plan.transmitted_pixels<<','<<plan.patches.size()<<','<<msu2::update_kind_name(plan.kind)<<','<<unchanged<<'\n';
                if(!csv)throw std::runtime_error("stream CSV write failed");
            }
            if(done-last_report>=std::chrono::seconds(1)) {
                std::cout<<"captured="<<produced.load()<<" host_frames_sent="<<sent<<" waiting_frames_replaced="<<queue.dropped()
                         <<" processed="<<processed<<" unchanged_frames="<<unchanged<<'\n';
                if(csv.is_open()){csv.flush();if(!csv)throw std::runtime_error("stream CSV flush failed");}
                last_report=done;
            }
        }
    } catch(...) {stop=true;queue.close();producer.join();throw;}
    stop=true;queue.close();producer.join();
    if(csv.is_open()){csv.flush();if(!csv)throw std::runtime_error("stream CSV flush failed");}
    double seconds=std::chrono::duration<double>(Clock::now()-start).count();
    std::cout<<"captured="<<produced.load()<<" host_frames_sent="<<sent<<" waiting_frames_replaced="<<queue.dropped()
             <<" bytes="<<wire<<" elapsed_s="<<seconds<<" host_send_fps="<<(sent/seconds)
             <<" processed="<<processed<<" unchanged_frames="<<unchanged<<" full_frames="<<full_frames
             <<" partial_frames="<<partial_frames<<" rectangles_sent="<<rectangles_sent
             <<"; this is NOT measured LCD refresh FPS\n";
}
void stream_adaptive(const Args& a) {
    const auto mode=encoding(a);
    const auto options=session_options(a);
    const auto count=a.num("--frames",300,1,10000000);
    const auto fps=a.num("--fps",30,1,240);
    const auto port=a.required("--port");
    const auto update=a.get("--update","full");
    if(update!="full" && update!="diff")throw std::invalid_argument("--update must be full or diff");
    const bool differential=update=="diff";
    if(a.has("--keyframe-ms") && !differential)
        throw std::invalid_argument("--keyframe-ms requires --update diff");
    const auto keyframe_ms=a.num("--keyframe-ms",1000,0,60000);
    const auto priority=msu2::parse_priority_mode(a.get("--priority-mode","off"));
    if(priority==msu2::PriorityMode::off)throw std::logic_error("adaptive stream requires a priority mode");
    if(a.has("--desktop"))throw std::runtime_error("Linux candidate does not support desktop capture");
    {Source validate_source(a);}
    check_csv_path(a);

    msu2::SerialTransport transport(port);
    msu2::Session session(transport,options);
    connection(session,a);
    std::ofstream csv;
    if(a.has("--csv")) {
        csv.open(a.required("--csv"));if(!csv)throw std::runtime_error("cannot create stream CSV");
        csv<<"frame_id,capture_ms,convert_ms,encode_ms,window_ack_ms,payload_write_ms,host_latency_ms,wire_bytes,write_calls,dropped_total,changed_pixels,transmitted_pixels,rectangles,update_kind,unchanged_total"
              ",priority_mode,active_layout,region,region_slot,region_outcome,mode_event,trigger,elapsed_ms,mode_duration_ms,transitions_total,predicted_full_ms,predicted_selected_ms,source_gap"
              ",region_due,region_sent,region_unchanged,region_skipped,region_missed_queue,region_max_miss_streak,region_max_stale_ms,region_last_synced_frame_id"
              ",center_due,center_sent,center_missed_queue,center_last_synced_frame_id,left_due,left_sent,left_skipped,left_missed_queue,left_max_streak,left_last_synced_frame_id,right_due,right_sent,right_skipped,right_missed_queue,right_max_streak,right_last_synced_frame_id\n";
    }
    std::cout<<"update="<<update<<" encoding="<<(mode==msu2::Encoding::raw?"raw":"pair")
             <<" target_fps="<<fps<<" priority_mode="<<msu2::priority_mode_name(priority)
             <<" keyframe_ms="<<(differential?keyframe_ms:0)<<" chunk="<<options.write_chunk<<'\n';
    msu2::LatestFrame<Captured> queue;
    std::atomic<bool> stop{false};
    std::atomic<unsigned> produced{0};
    const auto period=std::chrono::nanoseconds(1000000000ull/fps);
    const auto start=Clock::now();
    std::thread producer([&]{
        try {
            Source source(a);
            auto next=Clock::now();
            for(unsigned i=0;i<count && !stop.load() && !interrupted;++i) {
                const auto before=Clock::now();auto image=source.get(i);const auto after=Clock::now();
                queue.put(Captured{i,std::move(image),after,ms(after-before)});++produced;
                next+=period;
                if(next<Clock::now())next=Clock::now();
                if(i+1<count)std::this_thread::sleep_until(next);
            }
            queue.close();
        } catch(...) {queue.close(std::current_exception());}
    });
    unsigned sent=0,processed=0,unchanged=0,full_frames=0,partial_frames=0;
    unsigned last_processed_id=0;
    std::size_t wire=0,rectangles_sent=0;
    msu2::DeltaPlanner planner;
    msu2::PriorityScheduler scheduler(priority,fps);
    msu2::HostCostEstimator estimator;
    std::vector<std::uint8_t> rgb565,committed,cropped;
    auto last_report=start,last_full=start;
    try {
        Captured frame;
        while(!interrupted && queue.take(frame)) {
            const auto before=Clock::now();
            msu2::to_rgb565(frame.image,rgb565);
            const auto converted=Clock::now();
            const bool keyframe=differential && keyframe_ms &&
                converted-last_full>=msu2::Milliseconds(keyframe_ms);
            // Load recovery is judged against the regular full/diff work. A
            // due maintenance keyframe is still transmitted in full below.
            auto full_plan=planner.plan(rgb565,mode,!differential);
            msu2::UpdatePlan maintenance_plan;
            if(keyframe) maintenance_plan=planner.plan(rgb565,mode,true);
            const auto full_planned=Clock::now();
            const auto preparation_ms=frame.capture_ms+ms(full_planned-before);
            const auto full_prediction=estimator.predict(full_plan.wire_bytes,full_plan.patches.size(),
                                                         preparation_ms);
            const auto maintenance_prediction=keyframe ?
                estimator.predict(maintenance_plan.wire_bytes,maintenance_plan.patches.size(),
                                  preparation_ms) : msu2::HostCostPrediction{};
            const auto selection=scheduler.select(frame.id,planner.has_reference(),full_prediction.valid,
                                                  full_prediction.ms,queue.dropped(),ms(full_planned-start),
                                                  keyframe,maintenance_prediction.valid,
                                                  maintenance_prediction.ms);
            const auto source_gap=processed ? frame.id-last_processed_id-1 : 0;
            msu2::UpdatePlan plan=keyframe ? std::move(maintenance_plan) : std::move(full_plan);
            msu2::TransferStats st{};
            msu2::HostCostPrediction selected_prediction;
            msu2::SlotOutcome outcome=msu2::SlotOutcome::failed;
            const char* kind="unchanged";
            bool completed=false;
            const char* failure_phase="prepare";
            Clock::time_point planned=full_planned,done;
            try {
                if(selection.partitioned && !keyframe) {
                    plan={};
                    plan.kind=msu2::UpdateKind::bounding_box;
                    plan.changed_pixels=crop_and_count(rgb565,committed,selection.region.rect,cropped);
                    if(plan.changed_pixels) {
                        msu2::Patch patch;
                        patch.rect=selection.region.rect;
                        msu2::encode_frame(cropped,patch.rect,mode,patch.frame);
                        plan.wire_bytes=patch.frame.wire_bytes();
                        plan.transmitted_pixels=std::size_t(patch.rect.width)*patch.rect.height;
                        plan.patches.push_back(std::move(patch));
                    }
                }
                planned=Clock::now();
                selected_prediction=estimator.predict(plan.wire_bytes,plan.patches.size(),
                                                      frame.capture_ms+ms(planned-before));
                const bool budget_skip=selection.partitioned && !keyframe && !plan.patches.empty() &&
                    scheduler.skip_side(selection,selected_prediction.valid,selected_prediction.ms,
                                        ms(planned-start));
                if(budget_skip) {
                    outcome=msu2::SlotOutcome::skipped;
                    kind="priority_skipped";
                    plan.patches.clear();plan.wire_bytes=0;plan.transmitted_pixels=0;
                } else if(plan.patches.empty()) {
                    outcome=msu2::SlotOutcome::unchanged;
                    kind=selection.partitioned?"priority_unchanged":"unchanged";
                } else {
                    const auto update_deadline=planned+options.io_timeout;
                    for(const auto& patch:plan.patches) {
                        if(Clock::now()>=update_deadline)
                            throw std::runtime_error("multi-rectangle update deadline exceeded");
                        failure_phase="send";
                        const auto part=session.send(patch.frame);
                        st.bytes+=part.bytes;st.write_calls+=part.write_calls;
                        st.window_ack_ms+=part.window_ack_ms;
                        st.payload_write_ms+=part.payload_write_ms;
                    }
                    failure_phase="commit";
                    if(selection.partitioned && !keyframe) {
                        commit_region(committed,rgb565,selection.region.rect);
                        planner.commit(committed);
                        kind="priority_region";
                    } else {
                        committed=rgb565;
                        planner.commit(rgb565);
                        kind=selection.partitioned?"priority_keyframe_full":msu2::update_kind_name(plan.kind);
                    }
                    outcome=msu2::SlotOutcome::sent;
                    estimator.observe(st.bytes,plan.patches.size(),st.window_ack_ms,st.payload_write_ms);
                }
                done=Clock::now();
                scheduler.complete(selection,outcome,frame.capture_ms+ms(done-before),ms(done-start),
                                   selection.partitioned && keyframe && outcome==msu2::SlotOutcome::sent);
                completed=true;
            } catch(...) {
                std::string failure="nonstandard_exception";
                try { throw; }
                catch(const std::exception& e) {failure=e.what();}
                catch(...) {}
                planner.invalidate();committed.clear();
                if(!completed)
                    scheduler.complete(selection,msu2::SlotOutcome::failed,
                                       frame.capture_ms+ms(Clock::now()-before),ms(Clock::now()-start));
                const bool ack_timeout=std::string(failure_phase)=="send" &&
                    failure.find("response timeout")!=std::string::npos;
                std::cerr<<"adaptive_failure frame_id="<<frame.id
                         <<" layout="<<msu2::priority_mode_name(selection.layout)
                         <<" region="<<selection.region.name
                         <<" class="<<(ack_timeout?"window_ack_timeout":"transfer_or_prepare_error")
                         <<" phase="<<failure_phase
                         <<" completed_wire_bytes="<<st.bytes
                         <<" waiting_frames_replaced="<<queue.dropped()
                         <<" reason="<<failure<<'\n';
                throw;
            }
            ++processed;last_processed_id=frame.id;
            if(outcome==msu2::SlotOutcome::unchanged) ++unchanged;
            else if(outcome==msu2::SlotOutcome::sent) {
                ++sent;wire+=st.bytes;rectangles_sent+=plan.patches.size();
                if(plan.kind==msu2::UpdateKind::full) {
                    ++full_frames;last_full=done;
                } else ++partial_frames;
            }
            if(csv.is_open()) {
                const auto& selected=selection.partitioned ?
                    scheduler.counters(selection.layout,selection.region_index) : msu2::RegionCounters{};
                const auto& c=scheduler.counters(msu2::PriorityMode::center3,0);
                const auto& l=scheduler.counters(msu2::PriorityMode::center3,1);
                const auto& r=scheduler.counters(msu2::PriorityMode::center3,2);
                csv<<frame.id<<','<<frame.capture_ms<<','<<ms(converted-before)<<','<<ms(planned-converted)<<','
                   <<st.window_ack_ms<<','<<st.payload_write_ms<<','<<ms(done-frame.captured_at)<<','
                   <<st.bytes<<','<<st.write_calls<<','<<queue.dropped()<<','<<plan.changed_pixels<<','
                   <<plan.transmitted_pixels<<','<<plan.patches.size()<<','<<kind<<','<<unchanged<<','
                   <<msu2::priority_mode_name(priority)<<','<<msu2::priority_mode_name(selection.layout)<<','
                   <<selection.region.name<<','<<(selection.partitioned?int(selection.region_index):-1)<<','
                   <<msu2::slot_outcome_name(outcome)<<','
                   <<(selection.switched?(selection.partitioned?"enter":"exit"):"none")<<','
                   <<selection.trigger<<','<<ms(done-start)<<','
                   <<((selection.partitioned || (selection.switched && !selection.partitioned))?
                       std::max(0.0,ms(done-start)-scheduler.entered_ms()):0.0)<<','
                   <<scheduler.transitions()<<','<<(full_prediction.valid?full_prediction.ms:-1.0)<<','
                   <<(selected_prediction.valid?selected_prediction.ms:-1.0)<<','<<source_gap<<','
                   <<selected.due<<','<<selected.sent<<','<<selected.unchanged<<','<<selected.skipped<<','
                   <<selected.missed_queue<<','<<selected.max_consecutive_missed<<','<<selected.max_stale_ms<<','
                   <<selected.last_synced_frame_id<<','
                   <<c.due<<','<<c.sent<<','<<c.missed_queue<<','<<c.last_synced_frame_id<<','
                   <<l.due<<','<<l.sent<<','<<l.skipped<<','<<l.missed_queue<<','<<l.max_consecutive_missed<<','<<l.last_synced_frame_id<<','
                   <<r.due<<','<<r.sent<<','<<r.skipped<<','<<r.missed_queue<<','<<r.max_consecutive_missed<<','<<r.last_synced_frame_id<<'\n';
                if(!csv)throw std::runtime_error("stream CSV write failed");
            }
            if(done-last_report>=std::chrono::seconds(1)) {
                std::cout<<"captured="<<produced.load()<<" host_updates_sent="<<sent
                         <<" waiting_frames_replaced="<<queue.dropped()
                         <<" processed="<<processed<<" active_layout="
                         <<msu2::priority_mode_name(scheduler.active_layout())<<'\n';
                if(csv.is_open()){csv.flush();if(!csv)throw std::runtime_error("stream CSV flush failed");}
                last_report=done;
            }
        }
    } catch(...) {stop=true;queue.close();producer.join();throw;}
    stop=true;queue.close();producer.join();
    if(csv.is_open()){csv.flush();if(!csv)throw std::runtime_error("stream CSV flush failed");}
    const double seconds=std::chrono::duration<double>(Clock::now()-start).count();
    std::cout<<"captured="<<produced.load()<<" host_updates_sent="<<sent
             <<" waiting_frames_replaced="<<queue.dropped()<<" bytes="<<wire
             <<" elapsed_s="<<seconds<<" host_update_fps="<<(sent/seconds)
             <<" processed="<<processed<<" unchanged_regions="<<unchanged
             <<" full_frames="<<full_frames<<" region_or_partial_updates="<<partial_frames
             <<" rectangles_sent="<<rectangles_sent<<" mode_transitions="<<scheduler.transitions()
             <<"; LCD refresh FPS not measured\n";
    for(std::size_t i=0;i<msu2::priority_regions(priority).size();++i) {
        const auto& region=msu2::priority_regions(priority)[i];
        const auto& c=scheduler.counters(priority,i);
        std::cout<<"region="<<region.name<<" due="<<c.due<<" sent="<<c.sent
                 <<" unchanged="<<c.unchanged<<" skipped="<<c.skipped
                 <<" missed_queue="<<c.missed_queue<<" failed="<<c.failed
                 <<" max_miss_streak="<<c.max_consecutive_missed
                 <<" max_stale_ms="<<c.max_stale_ms
                 <<" last_source_frame_id="<<c.last_synced_frame_id<<'\n';
    }
}
double percentile(std::vector<double> values,double p) {
    std::sort(values.begin(),values.end());
    return values[std::min(values.size()-1,std::size_t(p*double(values.size()-1)))];
}
void bench(const Args& a) {
    check_csv_path(a);
    auto count=a.num("--frames",2000,10,100000);
    std::ofstream csv;
    const char* header="pattern,mode,frames,convert_ms_mean,encode_ms_mean,encode_ms_p50,encode_ms_p95,wire_bytes_mean,codec_fps_not_display,checksum";
    if(a.has("--csv")){csv.open(a.required("--csv"));if(!csv)throw std::runtime_error("cannot create benchmark CSV");csv<<header<<'\n';}
    std::cout<<"CPU conversion/encoding only; 32 warmups; no capture, serial or LCD.\n"<<header<<'\n';
    for(const auto& pattern:{"solid","bars","checker","gradient","noise","moving"}) {
        std::vector<msu2::Image> images;
        for(unsigned i=0;i<8;++i)images.push_back(msu2::make_pattern(pattern,i));
        for(auto mode:{msu2::Encoding::raw,msu2::Encoding::pair}) {
            std::vector<std::uint8_t> rgb;
            msu2::EncodedFrame frame;
            for(unsigned i=0;i<32;++i) {msu2::to_rgb565(images[i%8],rgb);msu2::encode_frame(rgb,{},mode,frame);}
            double convert=0,encode=0;
            std::vector<double> times;times.reserve(count);
            std::uint64_t bytes=0,checksum=0;
            for(unsigned i=0;i<count;++i) {
                auto t0=Clock::now();msu2::to_rgb565(images[i%8],rgb);auto t1=Clock::now();
                msu2::encode_frame(rgb,{},mode,frame);auto t2=Clock::now();
                convert+=ms(t1-t0);encode+=ms(t2-t1);times.push_back(ms(t2-t1));bytes+=frame.wire_bytes();
                checksum+=frame.payload[(i*17ull)%frame.payload.size()]+frame.payload.size()+rgb[(i*13ull)%rgb.size()];
            }
            auto output=[&](std::ostream& out) {
                out<<pattern<<','<<(mode==msu2::Encoding::raw?"raw":"pair")<<','<<count<<','<<convert/count<<','<<encode/count<<','
                   <<percentile(times,0.5)<<','<<percentile(times,0.95)<<','<<double(bytes)/count<<','
                   <<1000.0*count/(convert+encode)<<','<<checksum<<'\n';
            };
            output(std::cout);if(csv.is_open())output(csv);
        }
    }
    if(csv.is_open()){csv.flush();if(!csv)throw std::runtime_error("benchmark CSV write failed");}
}
void diff_bench(const Args& a) {
    check_csv_path(a);
    const auto count=a.num("--frames",300,1,100000);
    std::ofstream csv;
    const char* header="pattern,mode,frames,full_wire_bytes,diff_wire_bytes,saved_percent,changed_pixels,transmitted_pixels,rectangles,unchanged_frames,full_prepare_ms_mean,diff_prepare_ms_mean";
    if(a.has("--csv")){csv.open(a.required("--csv"));if(!csv)throw std::runtime_error("cannot create diff benchmark CSV");csv<<header<<'\n';}
    std::cout<<"Offline deterministic sequences; first frame full; no periodic keyframes, pacing or device I/O.\n"<<header<<'\n';
    for(const auto& pattern:{"solid","bars","checker","gradient","noise","moving"}) {
        for(auto mode:{msu2::Encoding::raw,msu2::Encoding::pair}) {
            msu2::DeltaPlanner full_planner,diff_planner;
            std::uint64_t full_bytes=0,diff_bytes=0,changed=0,pixels=0,rectangles=0,unchanged=0;
            double full_time=0,diff_time=0;
            std::vector<std::uint8_t> rgb;
            for(unsigned i=0;i<count;++i) {
                msu2::to_rgb565(msu2::make_pattern(pattern,i),rgb);
                auto t0=Clock::now();auto full=full_planner.plan(rgb,mode,true);auto t1=Clock::now();
                auto diff=diff_planner.plan(rgb,mode);auto t2=Clock::now();
                full_time+=ms(t1-t0);diff_time+=ms(t2-t1);
                full_bytes+=full.wire_bytes;diff_bytes+=diff.wire_bytes;
                changed+=diff.changed_pixels;pixels+=diff.transmitted_pixels;rectangles+=diff.patches.size();
                unchanged+=diff.patches.empty();
                full_planner.commit(rgb);diff_planner.commit(rgb);
            }
            auto output=[&](std::ostream& out) {
                out<<pattern<<','<<(mode==msu2::Encoding::raw?"raw":"pair")<<','<<count<<','<<full_bytes<<','<<diff_bytes<<','
                   <<(100.0*(1.0-double(diff_bytes)/double(full_bytes)))<<','<<changed<<','<<pixels<<','<<rectangles<<','
                   <<unchanged<<','<<full_time/count<<','<<diff_time/count<<'\n';
            };
            output(std::cout);if(csv.is_open()){output(csv);if(!csv)throw std::runtime_error("diff benchmark CSV write failed");}
        }
    }
    if(csv.is_open()){csv.flush();if(!csv)throw std::runtime_error("diff benchmark CSV flush failed");}
}

}
int main(int argc,char** argv) {
    std::signal(SIGINT,interrupt_handler);
    std::cout<<std::fixed<<std::setprecision(4);
    try {
        const auto a=parse(argc,argv);
        if(a.command=="help"){help();return 0;}
        if(a.command=="ports") {
            auto ports=msu2::list_serial_ports();
            for(const auto& p:ports)std::cout<<p<<'\n';
            if(ports.empty())std::cout<<"(no candidate serial ports)\n";
        } else if(a.command=="bench")bench(a);
        else if(a.command=="diff-bench")diff_bench(a);
        else if(a.command=="stream") {
            const auto priority=msu2::parse_priority_mode(a.get("--priority-mode","off"));
            if(priority==msu2::PriorityMode::off)stream(a);
            else stream_adaptive(a);
        }
        else if(a.command=="probe") {
            auto options=session_options(a);
            msu2::SerialTransport transport(a.required("--port"));msu2::Session session(transport,options);connection(session,a);
        } else {
            Source source(a);auto im=source.get();
            if(a.command=="preview")msu2::write_ppm(a.required("--output"),im);
            else {
                auto mode=encoding(a);
                std::vector<std::uint8_t> rgb;msu2::to_rgb565(im,rgb);
                msu2::EncodedFrame encoded;msu2::encode_frame(rgb,{},mode,encoded);
                if(a.command=="encode") {
                    save_commands(a.required("--output"),encoded);
                    std::cout<<"offline_command_stream_bytes="<<encoded.wire_bytes()<<" raw_pixels_bytes="<<encoded.raw_bytes<<" pages="<<encoded.pages<<'\n';
                } else if(a.command=="send") {
                    const auto hold_ms=a.num("--hold-open-ms",13,0,60000);
                    auto options=session_options(a);
                    msu2::SerialTransport transport(a.required("--port"));msu2::Session session(transport,options);
                    connection(session,a);print_stats(session.send(encoded));
                    std::cout<<"post_send_hold_requested_ms="<<hold_ms<<std::endl;
                    const auto hold_start=Clock::now();
                    const auto hold_end=hold_start+msu2::Milliseconds(hold_ms);
                    while(!interrupted && Clock::now()<hold_end)
                        std::this_thread::sleep_until(std::min(hold_end,Clock::now()+msu2::Milliseconds(50)));
                    std::cout<<"post_send_hold_elapsed_ms="<<ms(Clock::now()-hold_start)
                             <<" interrupted="<<(interrupted?1:0)<<std::endl;
                    if(interrupted)return 130;
                }
            }
        }
        return 0;
    } catch(const std::exception& e) {std::cerr<<"error: "<<e.what()<<'\n';return 1;}
}
