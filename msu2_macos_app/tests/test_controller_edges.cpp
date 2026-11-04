#include "msu2app/controller.hpp"
#include "msu2app/flash.hpp"
#include "msu2app/serial_guard.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void wait_until(F predicate, std::chrono::seconds timeout=5s) {
    const auto deadline=std::chrono::steady_clock::now()+timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now()>deadline) throw std::runtime_error("offline edge test wait timed out");
        std::this_thread::sleep_for(2ms);
    }
}
struct Shared {
    std::atomic<unsigned> opens{0}, leases{0}, windows{0}, reads{0}, erases{0}, commits{0};
    std::atomic<unsigned> write_calls{0}, idle_banners{0}, protocol_before_idle_banner{0};
    std::atomic<unsigned> fail_windows{0};
    std::atomic<bool> fail_every_window{false}, fail_backup{false}, hold_read{false}, read_started{false};
    std::atomic<bool> hold_window{false}, window_started{false}, hold_commit{false}, commit_started{false};
    std::atomic<bool> allow_idle_banner{false}, idle_read_started{false};
    std::atomic<unsigned> source_fail_at{1000000};
    std::atomic<unsigned> source_calls{0}, source_delay_ms{0};
    bool flash=false, sector_erase=false;
    std::mutex mutex;
    std::vector<std::pair<unsigned,std::vector<std::uint8_t>>> window_packets;
    std::vector<std::uint8_t> memory=std::vector<std::uint8_t>(msu2app::flash_total_bytes,0xAA);
};
struct Fake final:msu2::Transport {
    std::shared_ptr<Shared> shared;
    std::deque<std::uint8_t> incoming{0,'M','S','N','0','1'};
    std::array<std::uint8_t,256> staging{};
    std::thread::id owner=std::this_thread::get_id();
    std::chrono::steady_clock::time_point last_tx=std::chrono::steady_clock::now();
    bool idle_banner_delivered=false;
    unsigned instance;
    explicit Fake(std::shared_ptr<Shared> s):shared(std::move(s)),instance(++shared->opens) {}
    void require_owner() { check(owner==std::this_thread::get_id(),"transport must remain on its worker thread"); }
    std::size_t read(std::uint8_t* out,std::size_t capacity,msu2::Milliseconds timeout) override {
        require_owner();
        if(incoming.empty()&&std::chrono::steady_clock::now()-last_tx>=4500ms){
            shared->idle_read_started=true;
            if(shared->allow_idle_banner&&!idle_banner_delivered){
                incoming.insert(incoming.end(),{0,'M','S','N','0','1'});
                idle_banner_delivered=true;++shared->idle_banners;
            }
        }
        if(incoming.empty()) {std::this_thread::sleep_for(std::min(timeout,msu2::Milliseconds{1}));return 0;}
        const auto count=std::min(capacity,incoming.size());
        for(std::size_t i=0;i<count;++i){out[i]=incoming.front();incoming.pop_front();}
        return count;
    }
    void discard_input() override {require_owner();incoming.clear();}
    void reply(const std::uint8_t* bytes) {incoming.insert(incoming.end(),bytes,bytes+6);}
    void gate(std::atomic<bool>& held){
        const auto deadline=std::chrono::steady_clock::now()+5s;
        while(held){
            if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("fake operation gate was not released");
            std::this_thread::sleep_for(1ms);
        }
    }
    void write_all(const std::uint8_t* bytes,std::size_t count,msu2::Milliseconds) override {
        require_owner();
        ++shared->write_calls;
        if(std::chrono::steady_clock::now()-last_tx>=4500ms&&!idle_banner_delivered)
            ++shared->protocol_before_idle_banner;
        last_tx=std::chrono::steady_clock::now();idle_banner_delivered=false;
        if(count==6&&bytes[0]==0){reply(bytes);return;}
        if(count==18&&bytes[0]==2&&bytes[1]==0&&bytes[6]==2&&bytes[7]==1&&bytes[12]==2&&bytes[13]==3){
            ++shared->windows;
            {std::lock_guard<std::mutex> lock(shared->mutex);shared->window_packets.emplace_back(instance,std::vector<std::uint8_t>(bytes,bytes+count));}
            shared->window_started=true;gate(shared->hold_window);
            if(shared->fail_every_window)return;
            if(shared->fail_windows){--shared->fail_windows;return;}
            const msu2::Command response{2,3,7,0,0,0};reply(response.data());return;
        }
        if(!shared->flash)return;
        check(count%6==0,"flash command must be six-byte aligned");
        for(std::size_t i=0;i<count;i+=6){const auto* c=bytes+i;
            if(c[0]==4){check(c[1]<64,"flash staging index");std::copy_n(c+2,4,staging.begin()+c[1]*4);}
            else if(c[0]==3&&c[1]==0){
                const auto n=++shared->reads;
                shared->read_started=true;
                gate(shared->hold_read);
                if(shared->fail_backup&&n==10&&shared->erases==0)throw std::runtime_error("simulated backup read failure");
                const auto address=(unsigned(c[2])<<16)|(unsigned(c[3])<<8)|c[4];
                const msu2::Command response{3,0,c[2],c[3],c[4],shared->memory.at(address)};reply(response.data());
            } else if(c[0]==3&&c[1]==2){
                ++shared->erases;const auto page=(unsigned(c[2])<<8)|c[3];const auto pages=(unsigned(c[4])<<8)|c[5];
                const auto first=shared->sector_erase?(page/16)*16:page;
                const auto end=shared->sector_erase?((page+pages+15)/16)*16:page+pages;
                std::fill(shared->memory.begin()+first*256,shared->memory.begin()+end*256,0xFF);reply(c);
            } else if(c[0]==3&&c[1]==3){
                ++shared->commits;const auto page=(unsigned(c[2])<<16)|(unsigned(c[3])<<8)|c[4];
                std::copy(staging.begin(),staging.end(),shared->memory.begin()+page*256);
                shared->commit_started=true;gate(shared->hold_commit);reply(c);
            }
        }
    }
};
msu2app::Dependencies dependencies(const std::shared_ptr<Shared>& shared){
    msu2app::Dependencies d;
    d.lease=[shared](const std::string&)->std::shared_ptr<void>{
        ++shared->leases;
        return std::shared_ptr<void>(new int(0),[shared](void* p){--shared->leases;delete static_cast<int*>(p);});
    };
    d.transport=[shared](const std::string&)->std::unique_ptr<msu2::Transport>{return std::make_unique<Fake>(shared);};
    d.source=[shared](const msu2app::Settings& s,unsigned id){
        if(id>=shared->source_fail_at)throw std::runtime_error("simulated source capture failure");
        std::this_thread::sleep_for(std::chrono::milliseconds(shared->source_delay_ms.load()));
        ++shared->source_calls;
        if(s.prepared_image)return *s.prepared_image;
        return msu2::make_pattern("moving",id);
    };
    return d;
}
msu2app::Settings settings(){
    msu2app::Settings s;s.port="/dev/offline-edge-msu2";s.source="moving";s.fps=30;s.timeout_ms=20;s.hold_open_ms=0;return s;
}
std::string read_file(const std::filesystem::path& path){
    std::ifstream input(path,std::ios::binary);check(bool(input),"expected evidence file missing");
    return std::string(std::istreambuf_iterator<char>(input),{});
}
void test_pause_waiting(const std::string& root){
    auto shared=std::make_shared<Shared>();msu2app::Controller app(root,dependencies(shared));auto s=settings();s.fps=1;
    app.start_stream(s);wait_until([&]{return app.snapshot().sent==1;});
    // Consumer is waiting for the second frame; the next capture slot is 1 s away.
    std::this_thread::sleep_for(50ms);app.pause(true);
    wait_until([&]{return app.snapshot().paused;},1s);
    check(app.snapshot().state=="paused","UI must expose paused while the producer is idle");
    const auto sent=app.snapshot().sent;
    std::this_thread::sleep_for(50ms);check(app.snapshot().sent==sent,"pause cannot send another frame");
    // This is the UI's exact toggle operation; it must resume successfully.
    app.pause(!app.snapshot().paused);wait_until([&]{return app.snapshot().sent>sent;},3s);
    app.stop();wait_until([&]{return !app.snapshot().busy;});
    check(app.snapshot().connected&&shared->leases==1,"stream stop retains an exclusive connection");
    app.disconnect();wait_until([&]{return shared->leases==0;});
}
void test_failure_csv(const std::string& root){
    auto shared=std::make_shared<Shared>();shared->fail_windows=1;
    msu2app::Controller app(root,dependencies(shared));app.start_stream(settings());wait_until([&]{return !app.snapshot().busy;});
    check(app.snapshot().ack_timeouts==1&&!app.snapshot().connected,"failed window must be classified as ACK timeout");
    std::ifstream csv(std::filesystem::path(app.snapshot().log_directory)/"stream_1.csv");
    check(bool(csv),"stream CSV evidence missing");std::string line;
    check(bool(std::getline(csv,line)),"CSV header missing");
    const auto columns=std::count(line.begin(),line.end(),',')+1;
    check(columns==22,"CSV header must expose all 22 documented columns");
    unsigned rows=0;
    while(std::getline(csv,line)){++rows;check(std::count(line.begin(),line.end(),',')+1==columns,"failed CSV row must retain every header column");}
    check(rows==1,"failed first frame must appear in the evidence");
}
void test_reconnect_reference(const std::string& root){
    auto shared=std::make_shared<Shared>();shared->fail_windows=1;auto s=settings();s.auto_reconnect=true;s.priority="center3";
    msu2app::Controller app(root,dependencies(shared));app.start_stream(s);
    wait_until([&]{return app.snapshot().reconnects==1&&app.snapshot().sent>=3;});
    app.stop();wait_until([&]{return !app.snapshot().busy;});
    const auto state=app.snapshot();
    check(state.connected&&state.ack_timeouts==1&&state.handshake_failures==0&&state.transport_errors==0,"successful reconnect preserves error categories");
    check(shared->opens==2&&shared->leases==1,"reconnect replaces the lease without leaking it");
    std::lock_guard<std::mutex> lock(shared->mutex);
    const auto first=std::find_if(shared->window_packets.begin(),shared->window_packets.end(),[](const auto& packet){return packet.first==2;});
    check(first!=shared->window_packets.end(),"reconnect must send a frame");
    check(first->second[2]==0&&first->second[3]==0&&first->second[4]==0&&first->second[5]==0&&
          first->second[8]==0&&first->second[9]==160&&first->second[10]==0&&first->second[11]==80,
          "first frame after reconnect must refresh the complete reference");
}
void test_reconnect_bound(const std::string& root){
    auto shared=std::make_shared<Shared>();shared->fail_every_window=true;auto s=settings();s.auto_reconnect=true;
    msu2app::Controller app(root,dependencies(shared));app.start_stream(s);
    wait_until([&]{return !app.snapshot().busy;},8s);
    check(shared->opens<=4,"a stream may attempt recovery at most three times total");
    check(!app.snapshot().connected&&shared->leases==0,"exhausted recovery must release the serial lease");
    check(app.snapshot().ack_timeouts>=1&&app.snapshot().handshake_failures==0,"successful handshake cannot erase ACK failure history");
}
void test_paused_reconnect(const std::string& root){
    auto shared=std::make_shared<Shared>();shared->fail_windows=1;shared->hold_window=true;
    auto s=settings();s.auto_reconnect=true;msu2app::Controller app(root,dependencies(shared));app.start_stream(s);
    wait_until([&]{return shared->window_started.load();});app.pause(true);shared->hold_window=false;
    wait_until([&]{const auto state=app.snapshot();return state.reconnects==1&&state.streaming;});
    check(app.snapshot().paused&&app.snapshot().state=="paused","reconnect must preserve an in-flight user pause in the UI");
    check(app.snapshot().sent==0,"paused reconnect cannot send automatically");
    app.pause(!app.snapshot().paused);wait_until([&]{return app.snapshot().sent>=1;});
    app.stop();wait_until([&]{return !app.snapshot().busy;});
}
void test_source_error(const std::string& root){
    auto shared=std::make_shared<Shared>();shared->source_fail_at=2;
    msu2app::Controller app(root,dependencies(shared));app.start_stream(settings());wait_until([&]{return !app.snapshot().busy;});
    check(app.snapshot().message.find("simulated source capture failure")!=std::string::npos,"producer source exception must reach the UI");
    check(app.snapshot().connected&&shared->leases==1,"a source error keeps the clean connection exclusively owned");
    check(app.snapshot().ack_timeouts==0&&app.snapshot().transport_errors==0,"source errors cannot be mislabeled as serial errors");
    shared->source_fail_at=1000000;app.send_image(settings());wait_until([&]{return !app.snapshot().busy;});
    check(shared->opens==1&&app.snapshot().connected,"the worker must remain usable after a producer exception");
}
void test_idle_renewal(const std::string& root){
    {
        auto shared=std::make_shared<Shared>();msu2app::Controller app(root,dependencies(shared));auto s=settings();
        app.connect(s);wait_until([&]{return !app.snapshot().busy;});
        std::this_thread::sleep_for(4700ms);const auto writes=shared->write_calls.load();app.send_image(s);
        wait_until([&]{return shared->idle_read_started.load();});std::this_thread::sleep_for(50ms);
        check(shared->write_calls==writes,"idle renewal must wait for a banner before transmitting protocol bytes");
        shared->allow_idle_banner=true;wait_until([&]{return !app.snapshot().busy;});
        check(app.snapshot().sent==1&&app.snapshot().connected&&app.snapshot().reconnects==1,"idle send must renew the same session and send successfully");
        check(shared->opens==1&&shared->leases==1&&shared->idle_banners==1&&shared->protocol_before_idle_banner==0,"idle renewal cannot reopen or transmit before the observed banner");
    }
    {
        auto shared=std::make_shared<Shared>();shared->allow_idle_banner=true;
        msu2app::Controller app(root,dependencies(shared));auto s=settings();s.priority="center3";
        app.start_stream(s);wait_until([&]{return app.snapshot().sent>=4;});app.pause(true);
        std::this_thread::sleep_for(100ms);const auto sent=app.snapshot().sent;const auto windows=shared->windows.load();
        std::this_thread::sleep_for(4700ms);app.pause(false);wait_until([&]{return app.snapshot().sent>sent;});
        app.stop();wait_until([&]{return !app.snapshot().busy;});
        check(shared->opens==1&&shared->idle_banners==1&&app.snapshot().reconnects==1,"long pause resumes with an observed banner on the retained descriptor");
        check(shared->protocol_before_idle_banner==0,"long pause must never send a stale display command before handshake");
        std::lock_guard<std::mutex> lock(shared->mutex);
        check(shared->window_packets.size()>windows,"long pause must eventually send a replacement frame");
        const auto& packet=shared->window_packets.at(windows).second;
        check(packet[2]==0&&packet[3]==0&&packet[4]==0&&packet[5]==0&&packet[9]==160&&packet[11]==80,"first frame after a long idle pause must establish a complete reference");
    }
    {
        auto shared=std::make_shared<Shared>();msu2app::Controller app(root,dependencies(shared));auto s=settings();
        app.connect(s);wait_until([&]{return !app.snapshot().busy;});
        std::this_thread::sleep_for(4700ms);const auto writes=shared->write_calls.load();app.send_image(s);
        wait_until([&]{return !app.snapshot().busy;},7s);
        const auto state=app.snapshot();
        check(!state.connected&&shared->leases==0&&state.sent==0,"missing idle banner must stop without display transmission");
        check(state.handshake_failures==1&&state.ack_timeouts==0&&state.transport_errors==0,"idle banner failure must retain its handshake error category");
        check(shared->write_calls==writes&&shared->protocol_before_idle_banner==0,"idle banner failure must not issue any probe or display command");
    }
}
void test_lifecycle_stress(const std::string& root){
    for(int i=0;i<100;++i){auto shared=std::make_shared<Shared>();msu2app::Controller app(root,dependencies(shared));}
    for(int i=0;i<100;++i){
        auto shared=std::make_shared<Shared>();msu2app::Controller app(root,dependencies(shared));
        app.connect(settings());wait_until([&]{return !app.snapshot().busy;});
        app.disconnect();wait_until([&]{return !app.snapshot().connected&&shared->leases==0;});
    }
}
void test_idle_recovery(const std::string& root, bool automatic){
    auto shared=std::make_shared<Shared>();auto s=settings();s.auto_reconnect=automatic;s.priority="center3";
    msu2app::Controller app(root,dependencies(shared));app.start_stream(s);
    wait_until([&]{return app.snapshot().sent>=3;});app.pause(true);
    wait_until([&]{return app.snapshot().paused;});std::this_thread::sleep_for(4900ms);
    const auto sent=app.snapshot().sent;app.pause(false);
    if(automatic){
        wait_until([&]{return app.snapshot().reconnects==1&&app.snapshot().sent>sent;},15s);
        app.stop();wait_until([&]{return !app.snapshot().busy;},8s);
        auto state=app.snapshot();
        check(state.connected&&shared->leases==1&&state.transport_errors==0&&state.handshake_failures==1&&state.ack_timeouts==0&&state.reconnects==1&&shared->opens==2,"idle failure should reconnect exactly once with handshake classification");
        const auto csv=read_file(std::filesystem::path(state.log_directory)/"stream_1.csv");
        std::istringstream lines(csv);std::string line;unsigned failures=0;
        while(std::getline(lines,line))if(line.find("idle_renewal_failure")!=std::string::npos){
            ++failures;check(line.find(",failed,")!=std::string::npos&&std::count(line.begin(),line.end(),',')==21,"idle failure must occupy a complete failed CSV row");
        }
        check(failures==1,"CSV must contain one independent idle renewal failure row");
        std::lock_guard<std::mutex> lock(shared->mutex);
        const auto first=std::find_if(shared->window_packets.begin(),shared->window_packets.end(),[](const auto& p){return p.first==2;});
        check(first!=shared->window_packets.end()&&first->second[2]==0&&first->second[3]==0&&first->second[4]==0&&first->second[5]==0&&first->second[8]==0&&first->second[9]==160&&first->second[10]==0&&first->second[11]==80,"idle recovery first window must be full");
    }else{
        wait_until([&]{return !app.snapshot().busy;},15s);auto state=app.snapshot();
        check(!state.connected&&state.handshake_failures==1&&state.ack_timeouts==0&&state.reconnects==0&&shared->opens==1,"disabled idle recovery must stop");
    }
}
void test_separated_recoveries(const std::string& root){
    auto shared=std::make_shared<Shared>();auto s=settings();s.auto_reconnect=true;
    msu2app::Controller app(root,dependencies(shared));app.start_stream(s);
    wait_until([&]{return app.snapshot().sent>=2;});
    for(unsigned i=1;i<=4;++i){
        shared->fail_windows=1;
        wait_until([&]{return app.snapshot().reconnects>=i;},10s);
        const auto sent=app.snapshot().sent;
        wait_until([&]{return app.snapshot().sent>sent;},10s);
    }
    app.stop();wait_until([&]{return !app.snapshot().busy;});
    check(app.snapshot().reconnects==4&&app.snapshot().ack_timeouts==4&&shared->opens==5,"four separated failures must recover four times");
}
void test_flash_backup(const std::string& root){
    auto s=settings();s.source="image";s.image_path="frozen-offline-image";
    s.prepared_image=std::make_shared<const msu2::Image>(msu2::make_pattern("bars"));
    {
        auto shared=std::make_shared<Shared>();shared->flash=true;shared->fail_backup=true;
        msu2app::Controller app(root,dependencies(shared));app.flash_image(s,3826);wait_until([&]{return !app.snapshot().busy;});
        check(shared->reads==10&&shared->erases==0&&shared->commits==0,"backup read failure must prevent every destructive command");
        check(!app.snapshot().connected&&shared->leases==0,"backup failure must release transport and lease");
        const auto manifest=read_file(std::filesystem::path(app.snapshot().log_directory)/"flash_1_page3826.txt");
        check(manifest.find("simulated backup read failure")!=std::string::npos,"backup failure must retain concrete evidence");
    }
    {
        auto shared=std::make_shared<Shared>();shared->flash=true;shared->hold_read=true;
        msu2app::Controller app(root,dependencies(shared));app.flash_image(s,3826);
        wait_until([&]{return shared->read_started.load();});app.disconnect();shared->hold_read=false;
        wait_until([&]{return !app.snapshot().busy&&shared->leases==0;});
        check(shared->erases==0&&shared->commits==0,"disconnect during backup must prevent erase and commit");
        check(!app.snapshot().connected,"disconnect during backup cannot leave ready state");
    }
    {
        auto shared=std::make_shared<Shared>();shared->flash=true;
        msu2app::Controller app(root,dependencies(shared));
        // Make the concrete backup path unwritable without relying on OS permissions.
        std::filesystem::create_directory(std::filesystem::path(app.snapshot().log_directory)/"flash_1_page3826_backup.bin");
        app.flash_image(s,3826);wait_until([&]{return !app.snapshot().busy;});
        check(shared->reads==28672&&shared->erases==0&&shared->commits==0,"backup file save failure must prevent erase even after successful reads");
        check(!app.snapshot().connected&&shared->leases==0,"backup file failure must release the device");
    }
    {
        auto shared=std::make_shared<Shared>();shared->flash=true;
        msu2app::Controller app(root,dependencies(shared));app.flash_image(s,3997);wait_until([&]{return !app.snapshot().busy;});
        check(shared->opens==0&&shared->leases==0&&shared->reads==0,"invalid page range must fail before claiming a device");
    }
    {
        auto shared=std::make_shared<Shared>();shared->flash=true;
        msu2app::Controller app(root,dependencies(shared));app.flash_image(s,3826);wait_until([&]{return !app.snapshot().busy;});
        check(app.snapshot().connected&&app.snapshot().flash_progress==1,"valid flash must finish with readback evidence");
        check(shared->erases==1&&shared->commits==100,"one confirmed image uses one erase and 100 committed pages");
        const auto folder=std::filesystem::path(app.snapshot().log_directory);
        const auto backup=read_file(folder/"flash_1_page3826_backup.bin");
        const auto intended=read_file(folder/"flash_1_page3826_intended.rgb565");
        const auto manifest=read_file(folder/"flash_1_page3826.txt");
        check(backup.size()==28672&&std::all_of(backup.begin(),backup.end(),[](char c){return static_cast<unsigned char>(c)==0xAA;}),"backup must include all seven touched physical sectors");
        check(intended.size()==25600&&manifest.find("neighbors_verified=1")!=std::string::npos,"image and neighbor readback must be recorded");
        std::vector<std::uint8_t> pixels;msu2::to_rgb565(*s.prepared_image,pixels);
        check(std::equal(pixels.begin(),pixels.end(),shared->memory.begin()+3826*256),"confirmed frozen pixels must equal flashed bytes");
        check(shared->memory[3826*256-1]==0xAA&&shared->memory[3926*256]==0xAA,"fake page erase retains neighbors; this does not prove physical preservation");
    }
    {
        auto shared=std::make_shared<Shared>();shared->flash=true;shared->hold_commit=true;
        msu2app::Controller app(root,dependencies(shared));app.flash_image(s,3826);
        wait_until([&]{return shared->commit_started.load();});app.disconnect();shared->hold_commit=false;
        wait_until([&]{return !app.snapshot().busy&&shared->leases==0;});
        check(shared->erases==1&&shared->commits==1,"disconnect must finish the in-flight page then stop further writes");
        const auto manifest=read_file(std::filesystem::path(app.snapshot().log_directory)/"flash_1_page3826.txt");
        check(manifest.find("pages_written=1")!=std::string::npos&&manifest.find("cancelled=1")!=std::string::npos,"partial flash cancellation must retain page accounting");
    }
    {
        auto shared=std::make_shared<Shared>();shared->flash=true;
        msu2app::Controller app(root,dependencies(shared));app.flash_image(s,0);wait_until([&]{return !app.snapshot().busy;});
        check(app.snapshot().connected&&app.snapshot().flash_progress==1,"sector-aligned page zero must skip the empty preceding neighbor read");
        check(shared->erases==1&&shared->commits==100,"page zero still writes exactly 100 pages");
        const auto manifest=read_file(std::filesystem::path(app.snapshot().log_directory)/"flash_1_page0.txt");
        check(manifest.find("neighbors_verified=1")!=std::string::npos,"aligned first page must complete neighbor verification");
    }
    {
        auto shared=std::make_shared<Shared>();shared->flash=true;shared->sector_erase=true;
        msu2app::Controller app(root,dependencies(shared));app.flash_image(s,3826);wait_until([&]{return !app.snapshot().busy;});
        check(shared->erases==1&&shared->commits==100,"sector simulation writes the complete requested image");
        check(!app.snapshot().connected&&shared->leases==0,"neighbor mismatch must stop and release the device");
        const auto folder=std::filesystem::path(app.snapshot().log_directory);
        const auto manifest=read_file(folder/"flash_1_page3826.txt");
        check(manifest.find("neighbors_verified=0")!=std::string::npos,"physical-sector simulation must detect neighboring data loss");
        check(read_file(folder/"flash_1_page3826_backup.bin").size()==28672,"neighbor failure retains the original sector backup");
    }
}
void test_guard_key(){
    // Exact SHA-256 prefix from Python's existing shared guard convention.
    check(msu2app::serial_lock_key("/dev/cu.offline-edge-msu2")=="ba2eef120cfe02572b8e","C++ lock key must match Python guard's SHA-256 prefix");
    check(msu2app::serial_lock_key("/dev/tty.offline-edge-msu2")==msu2app::serial_lock_key("/dev/cu.offline-edge-msu2"),"cu and tty aliases must share the same lease key");
}
void test_ui_telemetry(const std::string& root){
    {
        auto shared=std::make_shared<Shared>();shared->source_delay_ms=100;
        msu2app::Controller app(root,dependencies(shared));app.start_stream(settings());
        wait_until([&]{return app.snapshot().sent>=3;});
        app.stop();wait_until([&]{return !app.snapshot().busy;});
        const auto state=app.snapshot();
        check(state.capture_dropped>0,"slow source must report elapsed capture slots");
        check(state.queue_dropped==0,"a slow source with fast transport cannot be labeled as waiting-frame replacement");
        check(state.ack_timeouts==0&&state.handshake_failures==0&&state.transport_errors==0,"source cadence misses are not serial errors");
        const auto events=read_file(std::filesystem::path(state.log_directory)/"events.log");
        check(events.find("capture_slots_missed="+std::to_string(state.capture_dropped))!=std::string::npos,"final source count must be retained in evidence");
        std::cout<<"Telemetry slow source: capture_slots_missed="<<state.capture_dropped<<" waiting_frames_replaced="<<state.queue_dropped<<'\n';
    }
    {
        auto shared=std::make_shared<Shared>();shared->hold_window=true;
        auto s=settings();s.fps=10;s.timeout_ms=1000;
        msu2app::Controller app(root,dependencies(shared));app.start_stream(s);
        wait_until([&]{return shared->window_started.load();});
        // Calls increment before making the next image. Seven calls guarantee
        // five subsequent frames have already reached the waiting slot.
        wait_until([&]{return shared->source_calls>=7;});
        shared->hold_window=false;
        wait_until([&]{return app.snapshot().queue_dropped>=4&&app.snapshot().sent>=2;});
        app.stop();wait_until([&]{return !app.snapshot().busy;});
        const auto state=app.snapshot();
        check(state.capture_dropped==0,"fast source during a gated send must not count waiting-frame replacement as capture misses");
        check(state.queue_dropped>=4&&state.dropped>=4,"only waiting frames are replaced while the in-flight frame completes");
        check(state.ack_timeouts==0&&state.handshake_failures==0&&state.transport_errors==0,"waiting-frame replacements are not serial errors");
        const auto events=read_file(std::filesystem::path(state.log_directory)/"events.log");
        check(events.find("waiting_frames_replaced="+std::to_string(state.queue_dropped))!=std::string::npos,"final waiting-frame count must be retained in evidence");
        std::cout<<"Telemetry gated send: capture_slots_missed="<<state.capture_dropped<<" waiting_frames_replaced="<<state.queue_dropped<<" observed_gaps="<<state.dropped<<'\n';
    }
}
} // namespace
int main(int argc,char** argv){
    const std::string root=argc>1?argv[1]:"/tmp/msu2-controller-edge-tests";
    const std::string selected=argc>2?argv[2]:"all";
    try{
        if(selected=="all"||selected=="pause")test_pause_waiting(root);
        if(selected=="all"||selected=="csv")test_failure_csv(root);
        if(selected=="all"||selected=="reconnect")test_reconnect_reference(root);
        if(selected=="all"||selected=="bound")test_reconnect_bound(root);
        if(selected=="all"||selected=="paused-reconnect")test_paused_reconnect(root);
        if(selected=="all"||selected=="source")test_source_error(root);
        if(selected=="all"||selected=="idle")test_idle_renewal(root);
        if(selected=="all"||selected=="lifecycle-stress")test_lifecycle_stress(root);
        if(selected=="all"||selected=="idle-recovery")test_idle_recovery(root,true);
        if(selected=="all"||selected=="idle-no-recovery")test_idle_recovery(root,false);
        if(selected=="all"||selected=="separated-recoveries")test_separated_recoveries(root);
        if(selected=="all"||selected=="flash")test_flash_backup(root);
        if(selected=="all"||selected=="guard")test_guard_key();
        if(selected=="all"||selected=="telemetry")test_ui_telemetry(root);
        std::cout<<"Controller edge tests passed: "<<selected<<" (fake transport/source; no serial device opened)\n";return 0;
    }catch(const std::exception& error){std::cerr<<"Controller edge test failed ["<<selected<<"]: "<<error.what()<<'\n';return 1;}
}
