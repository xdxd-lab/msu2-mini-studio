#include "msu2app/controller.hpp"
#include "msu2app/platform.hpp"
#include "msu2/trace.hpp"
#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>
using namespace std::chrono_literals;
namespace {
struct Gate {
 std::mutex m; std::condition_variable cv; int permit=0, entered=0, acks=0; bool closed=false;
 bool next(int& i){std::unique_lock<std::mutex> l(m);++entered;cv.notify_all();cv.wait(l,[&]{return permit>i||closed;});if(closed)return false;++i;return true;}
 void wait(int& v,int n,const char* what){std::unique_lock<std::mutex> l(m);if(!cv.wait_for(l,8s,[&]{return v>=n||closed;} )||v<n)throw std::runtime_error(std::string("watchdog: ")+what);}
 void release(int n){std::lock_guard<std::mutex> l(m);permit=n;cv.notify_all();}
 void ack(){std::lock_guard<std::mutex> l(m);++acks;cv.notify_all();}
 void close(){std::lock_guard<std::mutex> l(m);closed=true;cv.notify_all();}
}; Gate gate;
struct GateUnblock { ~GateUnblock(){gate.close();} };
std::vector<std::string> csv(const std::string& s){std::vector<std::string> out;std::string x;bool q=false;for(size_t i=0;i<s.size();++i){char c=s[i];if(c=='"'){if(q&&i+1<s.size()&&s[i+1]=='"'){x+='"';++i;}else q=!q;}else if(c==','&&!q){out.push_back(x);x.clear();}else x+=c;}if(q)throw std::runtime_error("bad CSV quoting");out.push_back(x);return out;}
void check(bool b,const char*m){if(!b)throw std::runtime_error(m);}
}
namespace msu2app {
struct ScreenCapture::Impl{};
ScreenCapture::ScreenCapture():impl_(std::make_unique<Impl>()){}
ScreenCapture::~ScreenCapture()=default;
msu2::Image ScreenCapture::grab(SourceMetadata* m){static thread_local int i=0;if(!gate.next(i))return {};int sample=i-1;if(m){m->available=true;m->epoch=sample<3?1:2;m->sequence=sample<2?10:(sample==2?11:1);static std::int64_t cached[3]={};int slot=(sample<2?0:(sample==2?1:2));if(!cached[slot])cached[slot]=msu2::trace_now_ns()-1000000;m->callback_ns=cached[slot];}msu2::Image im;im.width=160;im.height=80;im.rgb.assign(160*80*3,42);return im;}
std::vector<Device> enumerate_devices(){return {};}
msu2::Image load_image(const std::string&){return {};}
void save_image_png(const std::string&,const msu2::Image&){}
bool capture_permission(){return true;}bool request_capture_permission(){return true;}
}
namespace {
struct Fake final:msu2::Transport {
 std::vector<uint8_t> rx{0,'M','S','N','0','1'};
 size_t read(uint8_t*d,size_t n,msu2::Milliseconds)override{if(rx.empty())return 0;size_t k=std::min(n,rx.size());for(size_t j=0;j<k;++j){d[j]=rx.front();rx.erase(rx.begin());}if(k>=2&&d[0]==2&&d[1]==3)gate.ack();return k;}
 void discard_input()override{rx.clear();}
 void write_all(const uint8_t*p,size_t n,msu2::Milliseconds)override{if(n==6&&p[0]==0)rx.insert(rx.end(),p,p+n);else if(n==18&&p[0]==2&&p[1]==0)rx.insert(rx.end(),{2,3,7,0,0,0});}
};
void verify(const std::string& dir){std::ifstream in(dir+"/diagnostics.csv");check(bool(in),"missing diagnostics csv");std::string line;check(bool(std::getline(in,line)),"missing header");check(line=="t_us,kind,stage,result,frame,patch,v0,v1,v2,v3,v4,v5,rx_len,rx_hex,detail","unexpected header");std::vector<std::vector<std::string>> rows;while(std::getline(in,line)){auto f=csv(line);check(f.size()==15,"CSV column count");rows.push_back(std::move(f));}const int epochs[]={1,1,1,2},seqs[]={10,10,11,1},reps[]={0,1,0,0};std::map<int,int> frame_sample;bool got_identity[4]={},got_cb[4]={},got_take[4]={},end=false;int ids=0,cbs=0,takes=0;for(const auto& f:rows){if(f[1]=="log"&&f[2]=="end")end=true;if(f[1]=="capture"&&f[2]=="identity"){int sample=ids++,frame=std::stoi(f[4]);check(sample<4,"too many identity rows");check(f[3]=="available"&&std::stoi(f[6])==epochs[sample]&&std::stoi(f[7])==seqs[sample]&&std::stoi(f[8])==reps[sample],"identity tuple mismatch");check(frame_sample.emplace(frame,sample).second,"duplicate identity frame");got_identity[sample]=true;}}for(const auto& f:rows){if((f[2]=="identity"&&f[1]=="capture")||(f[2]=="callback_to_host"&&f[1]=="frame")||(f[2]=="take"&&f[1]=="frame"))check(std::stoll(f[12])==0&&f[13].empty(),"RX fields should be empty for frame diagnostics");if(f[1]=="frame"&&f[2]=="callback_to_host"){int frame=std::stoi(f[4]);check(frame_sample.count(frame),"callback has no identity");int n=frame_sample[frame];check(f[3]=="ok"&&std::stoll(f[6])>=0,"callback age invalid");check(std::stoi(f[7])==epochs[n]&&std::stoi(f[8])==seqs[n]&&std::stoi(f[9])==reps[n],"callback identity mismatch");got_cb[n]=true;++cbs;}if(f[1]=="frame"&&f[2]=="take"){int frame=std::stoi(f[4]);check(frame_sample.count(frame),"take has no identity");int n=frame_sample[frame];check(std::stoll(f[6])>=0,"queue age invalid");got_take[n]=true;++takes;}}check(end,"missing log,end");check(ids==4&&cbs==4&&takes==4,"missing frame records");for(int i=0;i<4;++i)check(got_identity[i]&&got_cb[i]&&got_take[i],"frame diagnostics incomplete");}
}
int main(int argc,char**argv){try{if(argc!=2)throw std::runtime_error("usage: test_controller_metadata_positive LOG_ROOT");setenv("MSU2_DIAGNOSTICS","1",1);msu2app::Dependencies d;d.lease=[](const std::string&){return std::make_shared<int>(1);};d.transport=[](const std::string&){return std::make_unique<Fake>();};std::string dir;auto app=std::make_unique<msu2app::Controller>(argv[1],std::move(d));GateUnblock unblocking;msu2app::Settings s;s.port="/dev/offline-metadata";s.source="desktop";s.fps=1;s.differential=false;s.timeout_ms=100;app->start_stream(s);for(int i=1;i<=4;++i){int entered=i;gate.wait(gate.entered,entered,"grab entered");gate.release(i);gate.wait(gate.acks,i,"frame ACK");}dir=app->snapshot().log_directory;app->stop();gate.close();app.reset();verify(dir);std::cout<<"metadata positive controller diagnostics passed\n";return 0;}catch(const std::exception&e){gate.close();std::cerr<<"metadata positive failed: "<<e.what()<<"\n";return 1;}}
