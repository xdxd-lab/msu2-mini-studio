// Explicit hardware test driver. Never run from test.sh. Owns the application
// Controller with its built-in shared SerialGuard, one serial owner throughout.
#include "msu2app/controller.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
using Clock=std::chrono::steady_clock;
using namespace std::chrono_literals;
void done(msu2app::Controller& app){auto deadline=Clock::now()+15s;while(app.snapshot().busy){if(Clock::now()>deadline){app.stop();throw std::runtime_error("phase deadline");}std::this_thread::sleep_for(10ms);}auto s=app.snapshot();if(s.state=="error"||!s.connected)throw std::runtime_error(s.message);}
void report(msu2app::Controller& app,const char* phase){auto s=app.snapshot();std::cout<<"phase="<<phase<<" state="<<s.state<<" sent="<<s.sent<<" bytes="<<s.bytes<<" dropped="<<s.dropped<<" ack_timeouts="<<s.ack_timeouts<<" handshake_failures="<<s.handshake_failures<<" transport_errors="<<s.transport_errors<<" reconnects="<<s.reconnects<<" host_fps="<<s.host_fps<<" logs="<<s.log_directory<<std::endl;}
int main(int argc,char** argv){
    if(argc!=3){std::cerr<<"usage: hardware_session /dev/cu.PORT log-root\n";return 2;}
    msu2app::Controller app(argv[2]);msu2app::Settings s;s.port=argv[1];s.source="bars";
    try{
        app.connect(s);done(app);report(app,"connect");app.send_image(s);done(app);report(app,"send");
        s.source="moving";s.fps=30;app.start_stream(s);auto started=Clock::now();
        while(app.snapshot().busy&&!app.snapshot().streaming){if(Clock::now()-started>10s)throw std::runtime_error("stream startup deadline");std::this_thread::sleep_for(10ms);}
        started=Clock::now();while(app.snapshot().busy&&Clock::now()-started<5s)std::this_thread::sleep_for(10ms);
        if(!app.snapshot().busy)throw std::runtime_error(app.snapshot().message);
        app.pause(true);std::this_thread::sleep_for(150ms);auto paused=app.snapshot().sent;std::this_thread::sleep_for(250ms);
        if(app.snapshot().sent!=paused)throw std::runtime_error("pause did not settle");report(app,"pause");
        app.pause(false);std::this_thread::sleep_for(500ms);app.stop();done(app);report(app,"stream_stop");
        // Exercise paused/idle session expiration through actual user lifecycle.
        std::this_thread::sleep_for(5500ms);s.source="bars";app.send_image(s);done(app);report(app,"idle_send");
        app.disconnect();std::this_thread::sleep_for(100ms);report(app,"disconnect");return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<std::endl;report(app,"failure");app.stop();app.disconnect();return 1;}
}
