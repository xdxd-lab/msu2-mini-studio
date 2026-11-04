#include "msu2app/controller.hpp"
#include "msu2app/serial_guard.hpp"
#include "msu2/protocol.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <deque>
#include <iostream>
#include <thread>
using namespace std::chrono_literals;
struct Shared {std::atomic<unsigned> opens{0},leases{0},windows{0};std::atomic<bool> fail_ack{false},fail_banner{false},fail_open{false};};
struct Fake final:msu2::Transport {
    std::shared_ptr<Shared> state;std::deque<std::uint8_t> rx;std::thread::id owner=std::this_thread::get_id();
    explicit Fake(std::shared_ptr<Shared> s):state(std::move(s)){++state->opens;rx={0,'M','S','N','0','1'};}
    size_t read(std::uint8_t* dst,size_t cap,msu2::Milliseconds timeout)override{
        assert(owner==std::this_thread::get_id());if(state->fail_banner)throw std::runtime_error("simulated disconnect during handshake");
        if(rx.empty()){std::this_thread::sleep_for(std::min(timeout,msu2::Milliseconds(1)));return 0;}
        size_t n=std::min(cap,rx.size());for(size_t i=0;i<n;++i){dst[i]=rx.front();rx.pop_front();}return n;
    }
    void write_all(const std::uint8_t* p,size_t n,msu2::Milliseconds)override{
        assert(owner==std::this_thread::get_id());
        if(n==6&&p[0]==0){rx.insert(rx.end(),p,p+n);return;}
        if(n==18&&p[0]==2&&p[1]==0){++state->windows;if(!state->fail_ack)rx.insert(rx.end(),{2,3,7,0,0,0});}
    }
    void discard_input()override{assert(owner==std::this_thread::get_id());rx.clear();}
};
msu2app::Dependencies dependencies(std::shared_ptr<Shared> shared){
    msu2app::Dependencies d;
    d.lease=[shared](const std::string&)->std::shared_ptr<void>{++shared->leases;return std::shared_ptr<void>(new int(1),[shared](void* p){--shared->leases;delete static_cast<int*>(p);});};
    d.transport=[shared](const std::string&)->std::unique_ptr<msu2::Transport>{if(shared->fail_open)throw std::runtime_error("simulated open failure");return std::make_unique<Fake>(shared);};
    d.source=[](const msu2app::Settings&,unsigned id){return msu2::make_pattern("moving",id);};return d;
}
template<class F>void wait_until(F predicate){auto deadline=std::chrono::steady_clock::now()+5s;while(!predicate()){if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("test wait timed out");std::this_thread::sleep_for(5ms);}}
int main(int argc,char** argv){
    const std::string root=argc>1?argv[1]:"/tmp/msu2-controller-tests";
    msu2app::Settings settings;settings.port="/dev/offline-msu2";settings.source="moving";settings.timeout_ms=30;settings.hold_open_ms=13;
    {
        auto s=std::make_shared<Shared>();msu2app::Controller app(root,dependencies(s));
        app.connect(settings);wait_until([&]{return !app.snapshot().busy;});assert(app.snapshot().connected);assert(s->leases==1);
        app.send_image(settings);wait_until([&]{return !app.snapshot().busy;});assert(app.snapshot().sent==1);assert(s->opens==1);
        app.start_stream(settings);wait_until([&]{return app.snapshot().sent>=4;});assert(app.snapshot().streaming);
        app.pause(true);std::this_thread::sleep_for(100ms);auto sent=app.snapshot().sent;std::this_thread::sleep_for(120ms);assert(app.snapshot().sent==sent);
        app.pause(false);wait_until([&]{return app.snapshot().sent>sent;});app.stop();wait_until([&]{return !app.snapshot().busy;});assert(app.snapshot().connected);
        settings.priority="center3";app.start_stream(settings);wait_until([&]{return app.snapshot().sent>=4;});app.stop();wait_until([&]{return !app.snapshot().busy;});
        s->fail_ack=true;app.send_image(settings);wait_until([&]{return !app.snapshot().busy;});assert(app.snapshot().ack_timeouts==1);assert(!app.snapshot().connected);assert(s->leases==0);
        s->fail_ack=false;app.connect(settings);wait_until([&]{return !app.snapshot().busy;});assert(app.snapshot().connected);
        app.disconnect();wait_until([&]{return !app.snapshot().connected;});assert(s->leases==0);
    }
    {
        auto s=std::make_shared<Shared>();s->fail_open=true;msu2app::Controller app(root,dependencies(s));app.connect(settings);wait_until([&]{return !app.snapshot().busy;});assert(s->leases==0);assert(!app.snapshot().connected);
        s->fail_open=false;s->fail_banner=true;app.connect(settings);wait_until([&]{return !app.snapshot().busy;});assert(s->leases==0);assert(app.snapshot().handshake_failures==1);
        auto invalid=settings;invalid.fps=0;app.connect(invalid);assert(!app.snapshot().busy);assert(s->opens==1);
    }
    assert(msu2app::serial_lock_key("/dev/cu.usbmodem01234567891")==msu2app::serial_lock_key("/dev/tty.usbmodem01234567891"));
    std::cout<<"Controller offline lifecycle, stream, pause, ACK/handshake separation and lease tests passed\n";
}
