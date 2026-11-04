// Offline controller diagnostics integration test; no device or screen access.
#include "msu2app/controller.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
using namespace std::chrono_literals;
namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct Fake final:msu2::Transport {
    std::deque<std::uint8_t> rx{0,'M','S','N','0','1'};
    std::size_t read(std::uint8_t* dst,std::size_t cap,msu2::Milliseconds timeout) override {
        if(rx.empty()){std::this_thread::sleep_for(std::min(timeout,msu2::Milliseconds(1)));return 0;}
        auto n=std::min(cap,rx.size());for(std::size_t i=0;i<n;++i){dst[i]=rx.front();rx.pop_front();}return n;
    }
    void discard_input() override {rx.clear();}
    void write_all(const std::uint8_t* p,std::size_t n,msu2::Milliseconds) override {
        if(n==6&&p[0]==0)rx.insert(rx.end(),p,p+n);
        else if(n==18&&p[0]==2&&p[1]==0)rx.insert(rx.end(),{2,3,7,0,0,0});
    }
};
msu2app::Dependencies dependencies(){msu2app::Dependencies d;
    d.lease=[](const std::string&)->std::shared_ptr<void>{return std::make_shared<int>(1);};
    d.transport=[](const std::string&)->std::unique_ptr<msu2::Transport>{return std::make_unique<Fake>();};
    d.source=[](const msu2app::Settings&,unsigned id){return msu2::make_pattern("moving",id);};return d;
}
template<class F>void wait_until(F pred){auto end=std::chrono::steady_clock::now()+5s;while(!pred()){if(std::chrono::steady_clock::now()>end)throw std::runtime_error("controller wait timed out");std::this_thread::sleep_for(5ms);}}
std::vector<std::string> parse(const std::string& line){std::vector<std::string> fields;std::string field;bool quoted=false;
    for(std::size_t i=0;i<line.size();++i){char c=line[i];if(c=='"'){if(quoted&&i+1<line.size()&&line[i+1]=='"'){field+='"';++i;}else quoted=!quoted;}else if(c==','&&!quoted){fields.push_back(field);field.clear();}else field+=c;}
    check(!quoted,"unterminated CSV quote");fields.push_back(field);return fields;
}
std::string exercise(const std::string& root,bool enabled){
    if(enabled)setenv("MSU2_DIAGNOSTICS","1",1);else unsetenv("MSU2_DIAGNOSTICS");
    std::string dir;{
        msu2app::Controller app(root,dependencies());dir=app.snapshot().log_directory;
        msu2app::Settings s;s.port="/dev/offline-trace";s.source="moving";s.fps=30;s.timeout_ms=30;s.chunk=60;
        app.connect(s);wait_until([&]{return !app.snapshot().busy;});check(app.snapshot().connected,"connect failed");
        app.start_stream(s);wait_until([&]{return app.snapshot().sent>=2;});app.stop();wait_until([&]{return !app.snapshot().busy;});
        app.disconnect();wait_until([&]{return !app.snapshot().connected&&!app.snapshot().busy;});
        app.connect(s);wait_until([&]{return !app.snapshot().busy;});check(app.snapshot().connected,"second connect failed");
        app.disconnect();wait_until([&]{return !app.snapshot().connected&&!app.snapshot().busy;});
    }return dir;
}
void verify(const std::string& dir){
    std::ifstream in(dir+"/diagnostics.csv");check(bool(in),"missing diagnostics CSV");std::string line;check(bool(std::getline(in,line)),"missing CSV header");
    check(line=="t_us,kind,stage,result,frame,patch,v0,v1,v2,v3,v4,v5,rx_len,rx_hex,detail","unexpected CSV header");
    bool meta=false,connect=false,capture=false,frame=false,patch=false,ack=false,stream_end=false,log_end=false;unsigned rows=0, event_rows=0, handshakes=0;
    std::int64_t lost_total=0, end_written=-1, end_lost=-1, end_failed=-1;
    while(std::getline(in,line)){auto f=parse(line);check(f.size()==15,"CSV field count");++rows;
        check(!log_end,"row after log end");
        if(f[1]!="log")++event_rows;
        if(f[2]=="handshake_banner"||f[2]=="handshake"||f[2]=="handshake_echo"){
            check(f[4]=="-1"&&f[5]=="-1","handshake inherited frame or patch context");
            if(f[1]=="rx"&&f[2]=="handshake_banner"&&f[3]=="ok")++handshakes;
        }
        auto time=std::stoll(f[0]);check(time>=0,"negative event time");for(int i=4;i<=12;++i)(void)std::stoll(f[i]);
        auto rx=std::stoul(f[12]);check(rx<=6&&f[13].size()==2*rx,"invalid bounded response");
        if(rx){
            check(f[1]=="rx"&&f[3]=="ok","raw bytes only on identified successful RX");
            const bool banner=f[2]=="handshake_banner"&&f[13]=="004d534e3031";
            const bool echo=f[2]=="handshake_echo"&&f[13]=="004d534e434e";
            const bool ack=(f[2]=="window_ack"||f[2]=="orientation_ack")&&f[13]=="0203";
            check(banner||echo||ack,"unexpected raw RX bytes in CSV");
        }
        if(f[1]=="rxclass"||f[1]=="rxread")check(rx==0&&f[13].empty(),"RX classification must not expose bytes");
        if(f[1]=="session"&&f[2]=="meta"&&f[3]=="stream_begin"){meta=true;check(f[6]=="1"&&f[7]=="30"&&f[8]=="1"&&f[9]=="1"&&f[10]=="60"&&f[11]=="30","stream metadata values");check(f[14].find("source=moving")!=std::string::npos&&f[14].find("csv=stream_1.csv")!=std::string::npos,"stream metadata detail");}
        if(f[1]=="connect"&&f[2]=="open"&&f[3]=="begin"){connect=true;check(f[14]=="/dev/offline-trace","CSV quoted detail");}
        if(f[1]=="capture"&&f[2]=="source")capture=true;
        if(f[1]=="frame"&&f[2]=="done")frame=true;
        if(f[1]=="patch"&&f[2]=="rect"){patch=true;check(std::stoll(f[4])>=0&&std::stoll(f[5])>=0,"patch context");}
        if(f[1]=="rx"&&f[2]=="window_ack"&&f[3]=="ok")ack=true;
        if(f[1]=="stream"&&f[2]=="end")stream_end=true;
        if(f[1]=="log"&&f[2]=="lost"){lost_total+=std::stoll(f[6]);check(std::stoll(f[6])>0&&std::stoll(f[7])==lost_total,"loss accounting");}
        if(f[1]=="log"&&f[2]=="end"){log_end=true;end_written=std::stoll(f[6]);end_lost=std::stoll(f[7]);end_failed=std::stoll(f[8]);}
    }
    check(rows>0&&meta&&connect&&capture&&frame&&patch&&ack&&stream_end&&log_end,"missing expected trace events");
    check(handshakes==2,"expected two successful handshakes");
    check(end_written==event_rows&&end_lost==lost_total&&end_failed==0,"log end counts do not match rows");
}
}
int main(int argc,char** argv){try{
    std::string root=argc>1?argv[1]:"/tmp/msu2-controller-trace-"+std::to_string(getpid());
    const char* old=std::getenv("MSU2_DIAGNOSTICS");const bool had=old;const std::string saved=old?old:"";
    auto disabled=exercise(root,false);check(!std::filesystem::exists(disabled+"/diagnostics.csv"),"diagnostics should default off");
    auto enabled=exercise(root,true);verify(enabled);
    if(had)setenv("MSU2_DIAGNOSTICS",saved.c_str(),1);else unsetenv("MSU2_DIAGNOSTICS");
    std::cout<<"Controller diagnostics offline tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<"Controller diagnostics test failed: "<<e.what()<<'\n';return 1;}}
