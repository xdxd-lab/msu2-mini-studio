// Offline Session trace contract tests. No serial device, display, or pixels read.
#include "msu2/session.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using Bytes=std::vector<std::uint8_t>;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct Collector final:msu2::TraceSink {
    std::array<msu2::TraceEvent,128> events{};
    std::size_t count=0; bool overflow=false;
    void record(const msu2::TraceEvent& e) noexcept override {
        if(count==events.size()){overflow=true;return;}events[count++]=e;
    }
};
enum class Scenario { success, timeout, payload_failure, banner_timeout, echo_timeout };
struct Fake final:msu2::Transport {
    Scenario scenario;
    std::deque<std::uint8_t> incoming{0x91,0x92,0,'M','S','N','0','7',0x93};
    std::vector<Bytes> writes;
    unsigned discards=0;
    std::size_t read_limit=512;
    std::vector<std::size_t> read_sizes;
    explicit Fake(Scenario s):scenario(s){if(s==Scenario::banner_timeout)incoming.clear();}
    std::size_t read(std::uint8_t* dst,std::size_t cap,msu2::Milliseconds timeout) override {
        if(incoming.empty()){std::this_thread::sleep_for(timeout);return 0;}
        auto n=std::min({cap,incoming.size(),read_limit});read_sizes.push_back(n);for(std::size_t i=0;i<n;++i){dst[i]=incoming.front();incoming.pop_front();}return n;
    }
    void discard_input() override {++discards;incoming.clear();}
    void write_all(const std::uint8_t* p,std::size_t n,msu2::Milliseconds) override {
        writes.emplace_back(p,p+n);
        if(n==6&&std::equal(p,p+n,msu2::handshake_command.begin())) {
            if(scenario!=Scenario::echo_timeout)incoming.insert(incoming.end(),{0x94,0,'M','S','N','C','N',0x95});
        } else if(n==18) {
            if(scenario!=Scenario::timeout)incoming.insert(incoming.end(),{0x96,2,3,0x97});
        } else if(scenario==Scenario::payload_failure)throw std::runtime_error("synthetic write failure");
    }
};
struct Result { Fake fake; Collector trace; bool failed=false; int version=-1; bool connected=false; explicit Result(Scenario s):fake(s){} };
void run(Result& r,bool enabled,bool chunks){
    msu2::SessionOptions o;o.handshake_timeout=msu2::Milliseconds(30);o.io_timeout=msu2::Milliseconds(20);o.write_chunk=6;
    o.trace=enabled?&r.trace:nullptr;o.trace_chunks=chunks;
    msu2::Session session(r.fake,o);
    try{r.version=session.handshake();}catch(const std::runtime_error&){r.failed=true;r.connected=session.connected();return;}
    session.set_trace_context(42,3);
    msu2::EncodedFrame f;f.window={2,0,0,0,0,0,2,1,0,160,0,80,2,3,7,0,0,0};
    // Distinct artificial bytes, never real pixels. They must not be traced.
    f.payload={0xa1,0xb2,0xc3,0xd4,0xe5,0xf6,0x12,0x34,0x56,0x78,0x9a,0xbc};
    try{session.send(f);}catch(const std::runtime_error&){r.failed=true;}
    r.connected=session.connected();
}
const msu2::TraceEvent& find(const Collector& c,const char* kind,const char* stage,const char* result){
    for(std::size_t i=0;i<c.count;++i){const auto& e=c.events[i];if(std::strcmp(e.kind,kind)==0&&std::strcmp(e.stage,stage)==0&&std::strcmp(e.result,result)==0)return e;}
    throw std::runtime_error(std::string("missing event ")+kind+"/"+stage+"/"+result);
}
void common(const Collector& c){
    check(!c.overflow,"collector overflow");std::int64_t previous=0;
    for(std::size_t i=0;i<c.count;++i){const auto& e=c.events[i];
        check(e.t_ns>0&&e.t_ns>=previous,"timestamps must be positive and ordered");previous=e.t_ns;
        check(e.rx_len<=6,"response length must be bounded");
        if(std::strcmp(e.kind,"rxclass")!=0&&std::strcmp(e.kind,"rxread")!=0)
            for(std::size_t k=e.rx_len;k<e.rx.size();++k)check(e.rx[k]==0,"unidentified RX bytes must not be retained");
        check(std::strstr(e.detail.data(),"a1b2c3d4e5f6")==nullptr,"payload leaked in detail");
    }
    const auto& banner=find(c,"rx","handshake_banner","ok");
    const std::array<std::uint8_t,6> expected{0,'M','S','N','0','7'};
    check(banner.rx_len==6&&banner.rx==expected&&banner.v[1]==9&&banner.v[4]==7&&banner.v[5]==2,"banner recognition and offset");
    const auto& echo=find(c,"rx","handshake_echo","ok");
    check(echo.rx_len==6&&echo.rx==msu2::handshake_command&&echo.v[1]==8&&echo.v[4]==1&&echo.v[5]==1,"echo recognition and offset");
    const auto& io=find(c,"io","discard_input","bytes_unavailable");check(io.frame==42&&io.patch==3&&io.v[0]>=0,"discard context/timing");
    const auto& tx=find(c,"tx","window_write","ok");check(tx.frame==42&&tx.patch==3&&tx.v[0]==18&&tx.v[1]>=0&&tx.rx_len==0,"window context/timing");
}
void scenario(Scenario s,bool chunks){
    Result plain(s),traced(s);run(plain,false,false);run(traced,true,chunks);
    if(s==Scenario::banner_timeout||s==Scenario::echo_timeout){
        check(plain.failed&&traced.failed&&!plain.connected&&!traced.connected,"handshake failure outcome");
        check(plain.fake.writes==traced.fake.writes&&plain.fake.discards==traced.fake.discards,"handshake trace must preserve IO");
        const char* stage=s==Scenario::banner_timeout?"handshake_banner":"handshake_echo";
        const auto& event=find(traced.trace,"rx",stage,"timeout");
        check(event.rx_len==0&&event.t_ns>0&&event.v[0]>=0,"handshake timeout event");
        find(traced.trace,"fail",stage,"exception");return;
    }
    check(plain.version==7&&traced.version==7,"handshake version");
    check(plain.fake.writes==traced.fake.writes&&plain.fake.discards==traced.fake.discards,"trace must preserve TX and discard calls");
    check(plain.failed==traced.failed&&plain.connected==traced.connected,"trace must preserve outcome");
    check(plain.fake.discards==1,"one discard per send");common(traced.trace);
    if(s==Scenario::timeout){
        const auto& ack=find(traced.trace,"rx","window_ack","timeout");
        check(ack.frame==42&&ack.patch==3&&ack.rx_len==0&&ack.v[4]==-1&&ack.v[0]>=0,"timeout must expose no raw response");
        find(traced.trace,"fail","window_ack","exception");check(traced.failed&&!traced.connected,"timeout disconnects session");
    } else {
        const auto& ack=find(traced.trace,"rx","window_ack","ok");
        check(ack.frame==42&&ack.patch==3&&ack.rx_len==2&&ack.rx[0]==2&&ack.rx[1]==3&&ack.v[1]==4&&ack.v[4]==1&&ack.v[5]==1,"ACK recognized without unrelated bytes");
        if(s==Scenario::payload_failure){const auto& e=find(traced.trace,"fail","payload_write","exception");check(e.frame==42&&e.patch==3&&e.v[0]==0&&e.v[1]>=0&&std::string(e.detail.data())=="synthetic write failure","payload failure stage/context");check(traced.failed&&!traced.connected,"write failure disconnects session");}
        else {const auto& e=find(traced.trace,"payload","write","ok");check(e.frame==42&&e.patch==3&&e.v[0]==12&&e.v[1]>=0&&e.v[2]==2&&e.rx_len==0,"payload aggregate fields");if(chunks){const auto& chunk=find(traced.trace,"tx","payload_chunk","ok");check(chunk.v[0]==6&&chunk.v[2]==0&&chunk.v[3]==0&&chunk.rx_len==0,"chunk metadata only");}check(!traced.failed&&traced.connected,"successful send remains connected");}
    }
}
void split_responses(){
    Result plain(Scenario::success),traced(Scenario::success);
    plain.fake.read_limit=2;traced.fake.read_limit=2;
    run(plain,false,false);run(traced,true,false);
    check(!plain.failed&&!traced.failed&&plain.connected&&traced.connected&&plain.version==7&&traced.version==7,"split responses accepted");
    check(plain.fake.writes==traced.fake.writes&&plain.fake.discards==traced.fake.discards&&plain.fake.read_sizes==traced.fake.read_sizes,"trace must preserve split response IO");
    check(plain.trace.count==0,"disabled trace records no events");
    const auto& banner=find(traced.trace,"rx","handshake_banner","ok");
    const std::array<std::uint8_t,6> expected{0,'M','S','N','0','7'};
    check(banner.rx==expected&&banner.rx_len==6&&banner.v[1]==8&&banner.v[2]==4&&banner.v[4]==7&&banner.v[5]==2,"split banner accounting");
    const auto& echo=find(traced.trace,"rx","handshake_echo","ok");
    check(echo.rx==msu2::handshake_command&&echo.rx_len==6&&echo.v[1]==8&&echo.v[2]==4&&echo.v[4]==2&&echo.v[5]==0,"split echo accounting");
    const auto& ack=find(traced.trace,"rx","window_ack","ok");
    check(ack.rx_len==2&&ack.rx[0]==2&&ack.rx[1]==3&&ack.v[1]==4&&ack.v[2]==2&&ack.v[4]==1&&ack.v[5]==1,"split ACK accounting");
    check(find(traced.trace,"rxclass","handshake_banner","ok").v==std::array<std::int64_t,6>{8,1,0,0,4,0},"split banner classification");
    check(find(traced.trace,"rxclass","handshake_echo","ok").v==std::array<std::int64_t,6>{8,0,1,0,4,0},"split echo classification");
    check(find(traced.trace,"rxclass","window_ack","ok").v==std::array<std::int64_t,6>{4,0,0,1,2,0},"split ACK classification");
    for(std::size_t i=0;i<traced.trace.count;++i){const auto& e=traced.trace.events[i];
        check(e.rx_len<=e.rx.size(),"bounded identified response");
        if(std::strcmp(e.kind,"rxclass")!=0&&std::strcmp(e.kind,"rxread")!=0)
            for(std::size_t j=e.rx_len;j<e.rx.size();++j)check(e.rx[j]==0,"noise must not be retained");
        check(std::strstr(e.detail.data(),"a1b2c3d4e5f6")==nullptr,"payload leaked in detail");
    }
}
void classification_limits(){
    Result plain(Scenario::success),traced(Scenario::success);
    // The invalid banner, valid echo and consecutive ACK prefixes are noise
    // for the banner wait; every byte arrives in a separate read.
    const Bytes input={0,'M','S','N','x','7',0,'M','S','N','C','N',2,3,2,3};
    for(auto* r:{&plain,&traced}){
        r->fake.incoming.assign(input.begin(),input.end());
        r->fake.incoming.insert(r->fake.incoming.end(),40,0xa5);
        r->fake.incoming.insert(r->fake.incoming.end(),{0,'M','S','N','0','7'});
        r->fake.read_limit=1;
    }
    run(plain,false,false);run(traced,true,false);
    check(!plain.failed&&!traced.failed&&plain.connected&&traced.connected&&plain.version==7&&traced.version==7,"classification preserves success");
    check(plain.fake.writes==traced.fake.writes&&plain.fake.discards==traced.fake.discards&&plain.fake.read_sizes==traced.fake.read_sizes,"classification preserves IO");
    check(plain.trace.count==0&&!traced.trace.overflow,"disabled trace and collector capacity");
    const auto& cls=find(traced.trace,"rxclass","handshake_banner","ok");
    check(cls.v==std::array<std::int64_t,6>{62,1,1,2,32,30},"classification and read cap counts");
    std::size_t reads=0;std::int64_t previous=-1;
    for(std::size_t i=0;i<traced.trace.count;++i){const auto& e=traced.trace.events[i];
        if(std::strcmp(e.kind,"rxclass")==0||std::strcmp(e.kind,"rxread")==0){
            check(e.rx_len==0,"classification rows have no raw RX");
            const std::array<std::uint8_t,6> empty{},banner{0,'M','S','N','0','7'},ack{2,3,0,0,0,0};
            check(e.rx==empty||e.rx==banner||e.rx==msu2::handshake_command||e.rx==ack,"classification rows do not retain noise");
            check(e.detail[0]=='\0',"classification rows have no raw detail");
        }
        if(std::strcmp(e.kind,"rxread")==0&&std::strcmp(e.stage,"handshake_banner")==0){
            check(e.v[0]==static_cast<std::int64_t>(reads)&&e.v[1]>=previous&&e.v[2]==1,"read index, elapsed time and length");
            previous=e.v[1];++reads;
        }
    }
    check(reads==32,"per-read rows capped at 32");
}
void single_read_classification(){
    Result r(Scenario::success);run(r,true,false);
    check(!r.failed,"single-read success");
    for(const char* stage:{"handshake_banner","handshake_echo","window_ack"}){
        find(r.trace,"rxclass",stage,"ok");
        for(std::size_t i=0;i<r.trace.count;++i)
            check(std::strcmp(r.trace.events[i].kind,"rxread")!=0||std::strcmp(r.trace.events[i].stage,stage)!=0,"single-read success omits rxread");
    }
}
}
int main(){try{scenario(Scenario::banner_timeout,false);scenario(Scenario::echo_timeout,false);scenario(Scenario::success,false);scenario(Scenario::success,true);scenario(Scenario::timeout,true);scenario(Scenario::payload_failure,true);split_responses();classification_limits();single_read_classification();std::cout<<"Session trace offline tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<"Session trace test failed: "<<e.what()<<'\n';return 1;}}
