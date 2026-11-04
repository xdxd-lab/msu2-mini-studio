#include "msu2app/controller.hpp"
#include "msu2app/platform.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>
namespace {volatile std::sig_atomic_t interrupted=0;void stop(int){interrupted=1;}
unsigned number(const std::string& s,unsigned low,unsigned high){size_t n=0;auto v=std::stoul(s,&n);if(n!=s.size()||v<low||v>high)throw std::invalid_argument("numeric option outside range");return unsigned(v);}
}
int main(int argc,char** argv){
    try{
        if(argc<2||std::string(argv[1])=="--help"){
            std::cout<<"MSU2 Mini Studio 0.2.0 macOS C++ controller\n"
              "msu2ctl devices\nmsu2ctl connect|send|stream --port PATH [options]\n"
              "msu2ctl download --port PATH --start-page 0..3996 --output-dir DIR [--timeout-ms N] [--log-root DIR]\n"
              "--source desktop|image|moving|bars|gradient|checker|solid|noise (default moving)\n"
              "--input FILE --fps 1..60 --priority off|bands4|center3|halves2\n"
              "--duration SECONDS (stream, 0 means until Ctrl-C) --log-root DIR\n"
              "--hold-open-ms 0..60000 --timeout-ms 1..60000 --full --raw --reconnect\n"
              "Flash writes are available in the app after image/range confirmation.\n";return 0;
        }
        const std::string action=argv[1];if(action=="devices"){
            if(argc!=2)throw std::invalid_argument("devices accepts no options");
            for(const auto& d:msu2app::enumerate_devices())std::cout<<d.port<<"\t"<<d.label<<"\tserial="<<d.serial<<'\n';return 0;
        }
        if(action!="connect"&&action!="send"&&action!="stream"&&action!="download")throw std::invalid_argument("unknown command");
        std::map<std::string,std::string> options;
        for(int i=2;i<argc;++i){std::string k=argv[i];if(options.count(k))throw std::invalid_argument("duplicate option: "+k);
            if(k=="--full"||k=="--raw"||k=="--reconnect")options[k]="true";
            else{if(k!="--port"&&k!="--source"&&k!="--input"&&k!="--fps"&&k!="--priority"&&k!="--duration"&&k!="--log-root"&&k!="--hold-open-ms"&&k!="--timeout-ms"&&k!="--start-page"&&k!="--output-dir")throw std::invalid_argument("unknown option: "+k);
                if(i+1>=argc)throw std::invalid_argument("missing value: "+k);options[k]=argv[++i];}}
        msu2app::Settings s;s.source="moving";s.port=options["--port"];if(s.port.empty())throw std::invalid_argument("--port is required");
        if(options.count("--source"))s.source=options["--source"];if(options.count("--input")){s.image_path=options["--input"];s.source="image";}
        if(options.count("--fps"))s.fps=number(options["--fps"],1,60);if(options.count("--priority"))s.priority=options["--priority"];
        if(options.count("--hold-open-ms"))s.hold_open_ms=number(options["--hold-open-ms"],0,60000);if(options.count("--timeout-ms"))s.timeout_ms=number(options["--timeout-ms"],1,60000);
        s.differential=!options.count("--full");s.pair=!options.count("--raw");s.auto_reconnect=options.count("--reconnect");
        const unsigned duration=options.count("--duration")?number(options["--duration"],0,86400):0;
        if(action!="stream"&&options.count("--duration"))throw std::invalid_argument("--duration requires stream");
        unsigned page=0;
        if(action=="download"){
            for(const auto& option:options)if(option.first!="--port"&&option.first!="--start-page"&&option.first!="--output-dir"&&option.first!="--timeout-ms"&&option.first!="--log-root")throw std::invalid_argument("option not supported by download: "+option.first);
            if(!options.count("--start-page")||!options.count("--output-dir")||options["--output-dir"].empty())throw std::invalid_argument("download requires --start-page and --output-dir");
            page=number(options["--start-page"],0,3996);
        }else if(options.count("--start-page")||options.count("--output-dir"))throw std::invalid_argument("--start-page/--output-dir require download");
        std::signal(SIGINT,stop);std::signal(SIGTERM,stop);
        msu2app::Controller app(options["--log-root"]);
        if(action=="connect")app.connect(s);else if(action=="send")app.send_image(s);else if(action=="download")app.download_image(s,page,options["--output-dir"]);else app.start_stream(s);
        std::string previous;auto started=std::chrono::steady_clock::now();bool stream_started=false,issued_stop=false;
        for(;;){auto state=app.snapshot();if(state.message!=previous){std::cout<<state.state<<": "<<state.message<<std::endl;previous=state.message;}
            if(!state.busy)break;
            if(state.streaming&&!stream_started){started=std::chrono::steady_clock::now();stream_started=true;}
            if((interrupted||(duration&&stream_started&&std::chrono::steady_clock::now()-started>=std::chrono::seconds(duration)))&&!issued_stop){app.stop();issued_stop=true;}
            std::this_thread::sleep_for(std::chrono::milliseconds(20));}
        auto st=app.snapshot();std::cout<<"sent="<<st.sent<<" unchanged="<<st.unchanged<<" dropped="<<st.dropped<<" bytes="<<st.bytes<<" host_fps="<<st.host_fps<<" handshake_failures="<<st.handshake_failures<<" ack_timeouts="<<st.ack_timeouts<<" transport_errors="<<st.transport_errors<<" reconnects="<<st.reconnects<<"\nlogs="<<st.log_directory<<'\n';
        if(action=="download")std::cout<<"downloaded_bytes="<<st.download_bytes<<"\noutput="<<st.download_directory<<'\n';
        const bool ok=st.state!="error"&&st.connected&&(action!="download"||st.download_generation>0);app.disconnect();return interrupted?130:(ok?0:1);
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}
}
