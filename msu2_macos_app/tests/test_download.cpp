#include "msu2app/controller.hpp"
#include "msu2app/flash.hpp"
#include "msu2app/platform.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;
namespace fs=std::filesystem;
namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class F> void wait_for(F f){auto end=std::chrono::steady_clock::now()+10s;while(!f()){if(std::chrono::steady_clock::now()>end)throw std::runtime_error("download worker timed out");std::this_thread::sleep_for(2ms);}}
struct Shared {
    std::atomic<unsigned> opens{0},leases{0},reads{0},writes{0},erases{0},commits{0},windows{0},sources{0};
    std::atomic<int> fault{0}; // 1 wrong-address reply, 2 omit reply
    std::atomic<bool> hold{false},started{false};
    std::atomic<std::size_t> cancel_at{0};
    std::uint32_t start=0;
};
std::uint8_t byte_at(std::uint32_t address){return static_cast<std::uint8_t>((address*37u+address/13u+91u)&255u);}
struct Fake final:msu2::Transport {
    std::shared_ptr<Shared> s; std::deque<std::uint8_t> rx{0,'M','S','N','0','1'}; std::thread::id owner=std::this_thread::get_id();
    explicit Fake(std::shared_ptr<Shared> x):s(std::move(x)){++s->opens;}
    void own(){check(owner==std::this_thread::get_id(),"transport thread ownership changed");}
    std::size_t read(std::uint8_t* out,std::size_t n,msu2::Milliseconds timeout)override{own();if(rx.empty()){std::this_thread::sleep_for(std::min(timeout,msu2::Milliseconds{1}));return 0;}auto k=std::min(n,rx.size());for(std::size_t i=0;i<k;++i){out[i]=rx.front();rx.pop_front();}return k;}
    void discard_input()override{own();rx.clear();}
    void write_all(const std::uint8_t* p,std::size_t n,msu2::Milliseconds)override{
        own();++s->writes;
        if(n==6&&p[0]==0){rx.insert(rx.end(),p,p+n);return;}
        if(n==18&&p[0]==2){++s->windows;return;}
        check(n==6&&p[0]==3,"download may issue read commands only");
        if(p[1]==2){++s->erases;throw std::runtime_error("erase command observed");}
        if(p[1]==3){++s->commits;throw std::runtime_error("commit command observed");}
        check(p[1]==0,"unexpected flash command");
        const auto address=(std::uint32_t(p[2])<<16)|(std::uint32_t(p[3])<<8)|p[4];
        const auto index=++s->reads;s->started=true;
        while(s->hold){std::this_thread::sleep_for(1ms);}
        if(s->cancel_at&&index>=s->cancel_at) throw std::runtime_error("test transport reached cancellation point");
        if(s->fault==2&&index==1)return;
        const auto reply_address=address+((s->fault==1&&index==1)?1u:0u);
        const msu2::Command reply{3,0,static_cast<std::uint8_t>(reply_address>>16),static_cast<std::uint8_t>(reply_address>>8),static_cast<std::uint8_t>(reply_address),byte_at(address)};
        rx.insert(rx.end(),reply.begin(),reply.end());
    }
};
msu2app::Dependencies deps(const std::shared_ptr<Shared>& s){msu2app::Dependencies d;
    d.lease=[s](const std::string&){++s->leases;return std::shared_ptr<void>(new int(0),[s](void* p){--s->leases;delete static_cast<int*>(p);});};
    d.transport=[s](const std::string&){return std::make_unique<Fake>(s);};
    d.source=[s](const msu2app::Settings&,unsigned){++s->sources;return msu2::make_pattern("bars");};return d;}
msu2app::Settings settings(){msu2app::Settings x;x.port="/dev/offline-download";x.timeout_ms=15;x.hold_open_ms=0;return x;}
std::string contents(const fs::path& p){std::ifstream f(p,std::ios::binary);check(bool(f),"expected file missing");return {std::istreambuf_iterator<char>(f),{}};}
void run_download(const fs::path& root,unsigned page,std::shared_ptr<Shared> s){
    msu2app::Controller c(root.string(),deps(s));c.download_image(settings(),page,root.string());wait_for([&]{return !c.snapshot().busy;});
    const auto st=c.snapshot();check(st.state!="error","valid download failed");check(st.download_bytes==msu2app::flash_image_bytes&&st.download_progress==1,"download progress/byte count incomplete");
    check(st.download_generation==1&&!st.download_directory.empty(),"successful download snapshot missing");
    check(s->reads==msu2app::flash_image_bytes&&s->erases==0&&s->commits==0&&s->windows==0&&s->sources==0,"download issued a non-read operation");
    check(st.connected&&s->leases==1,"successful download should retain its usable session");
    const fs::path folder(st.download_directory);auto raw=contents(folder/"image.rgb565");check(raw.size()==msu2app::flash_image_bytes,"raw download size mismatch");
    for(std::size_t i=0;i<raw.size();++i)check(static_cast<std::uint8_t>(raw[i])==byte_at(page*256+static_cast<std::uint32_t>(i)),"raw bytes differ from requested addresses");
    auto manifest=contents(folder/"manifest.txt");const std::array<std::string,9> metadata={"start_page="+std::to_string(page),"end_page="+std::to_string(page+99),"start_byte="+std::to_string(page*256),"byte_count=25600","width=160","height=80","format=RGB565 big-endian, top-down","operation=read_only","status=complete"};for(const auto& expected:metadata)check(manifest.find(expected)!=std::string::npos,"manifest metadata missing");
    auto png=msu2app::load_image((folder/"image.png").string());check(png.width==160&&png.height==80,"PNG dimensions mismatch");std::vector<std::uint8_t> encoded;msu2::to_rgb565(png,encoded);check(encoded.size()==raw.size(),"PNG conversion size mismatch");for(std::size_t i=0;i<encoded.size();++i)if(encoded[i]!=static_cast<std::uint8_t>(raw[i]))throw std::runtime_error("PNG RGB565 roundtrip mismatch at byte "+std::to_string(i)+" source="+std::to_string(static_cast<unsigned char>(raw[i]))+" decoded="+std::to_string(encoded[i]));
}
void test_invalid(const fs::path& root){auto s=std::make_shared<Shared>();msu2app::Controller c(root.string(),deps(s));c.download_image(settings(),3997,root.string());wait_for([&]{return !c.snapshot().busy;});check(s->opens==0&&s->reads==0&&s->leases==0,"invalid page must fail before device access");check(c.snapshot().state=="error","invalid page must surface an error");
    auto s2=std::make_shared<Shared>();msu2app::Controller c2(root.string(),deps(s2));c2.download_image(settings(),0,(root/"missing").string());wait_for([&]{return !c2.snapshot().busy;});check(s2->opens==0&&s2->reads==0,"invalid output parent must fail before device access");}
void test_stale_success_cleared(const fs::path& parent){const auto root=parent/"stale-success";fs::create_directories(root);auto s=std::make_shared<Shared>();msu2app::Controller c(root.string(),deps(s));c.download_image(settings(),0,root.string());wait_for([&]{return !c.snapshot().busy;});check(!c.snapshot().download_directory.empty()&&!c.snapshot().download_preview.rgb.empty(),"successful download should publish output and preview");c.download_image(settings(),0,(root/"missing").string());wait_for([&]{return !c.snapshot().busy;});const auto after=c.snapshot();check(after.state=="error"&&after.download_directory.empty()&&after.download_preview.rgb.empty()&&after.download_bytes==0&&after.download_progress==0,"invalid next request must clear stale successful output");}
void test_failure(const fs::path& parent,int fault){const fs::path root=parent/(fault==1?"failure-wrong-address":"failure-timeout");fs::create_directories(root);auto s=std::make_shared<Shared>();s->fault=fault;msu2app::Controller c(root.string(),deps(s));c.download_image(settings(),0,root.string());wait_for([&]{return !c.snapshot().busy;});const auto st=c.snapshot();check(st.state=="error"&&!st.connected&&s->leases==0,"read failure must error and release the lease");check(s->erases==0&&s->commits==0&&s->windows==0&&s->sources==0,"failed download performed an unrelated operation");check(st.download_generation==0,"failed download must not publish a successful generation");for(const auto& e:fs::directory_iterator(root))if(e.is_directory()&&e.path().filename().string().rfind("MSU2_page",0)==0)check(!fs::exists(e.path()/"image.png")&&!fs::exists(e.path()/"image.rgb565"),"failed download left a completed asset");}
void test_cancel(const fs::path& parent){const fs::path root=parent/"cancelled";fs::create_directories(root);auto s=std::make_shared<Shared>();s->hold=true;msu2app::Controller c(root.string(),deps(s));c.download_image(settings(),0,root.string());wait_for([&]{return s->started.load();});c.disconnect();s->hold=false;wait_for([&]{return !c.snapshot().busy&&s->leases==0;});check(s->erases==0&&s->commits==0&&c.snapshot().download_generation==0,"cancelled download must not mutate flash or publish success");for(const auto& e:fs::directory_iterator(root))if(e.is_directory()&&e.path().filename().string().rfind("MSU2_page",0)==0)check(!fs::exists(e.path()/"image.png")&&!fs::exists(e.path()/"image.rgb565"),"cancel left a complete asset");}
}
int main(int argc,char** argv){try{if(argc<2)throw std::runtime_error("pass a project-local evidence directory");const fs::path base=argv[1];fs::create_directories(base);const fs::path root=base/("download-run-"+std::to_string(getpid()));fs::create_directories(root);test_invalid(root);run_download(root,0,std::make_shared<Shared>());run_download(root,3996,std::make_shared<Shared>());test_failure(root,1);test_failure(root,2);test_cancel(root);test_stale_success_cleared(root);std::cout<<"Download offline tests passed: boundaries, exact RGB565/PNG, read-only protocol, preflight, errors, cancellation, stale-output clearing\n";return 0;}catch(const std::exception& e){std::cerr<<"Download offline tests failed: "<<e.what()<<'\n';return 1;}}
