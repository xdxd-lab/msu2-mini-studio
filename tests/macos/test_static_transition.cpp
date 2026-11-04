#include "static_transition_trace.hpp"
#include "msu2/image.hpp"
#include <deque>
#include <sstream>
#include <thread>

namespace {
void require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
class Fake final:public msu2::Transport {
public:
    std::deque<std::uint8_t> rx;
    bool failed_state=false, advertise=true, wrong_recovery_echo=false;
    unsigned window_count=0, fail_window=0, handshake_count=0, payload_writes=0;
    Fake() {banner();}
    void add(const std::uint8_t* p,std::size_t n) {rx.insert(rx.end(),p,p+n);}
    void banner() {const std::uint8_t b[]={0,'M','S','N','0','1'};add(b,6);}
    std::size_t read(std::uint8_t* dst,std::size_t n,msu2::Milliseconds timeout) override {
        if(rx.empty()) {
            std::this_thread::sleep_for(std::min(timeout,msu2::Milliseconds(1)));
            if(failed_state && advertise) banner();
        }
        // Fragment banners and exact echoes to exercise cross-read matching.
        auto size=std::min<std::size_t>({n,rx.size(),2});
        for(std::size_t i=0;i<size;++i){dst[i]=rx.front();rx.pop_front();}
        return size;
    }
    void write_all(const std::uint8_t* src,std::size_t n,msu2::Milliseconds) override {
        if(n==6 && std::equal(msu2::handshake_command.begin(),msu2::handshake_command.end(),src)) {
            ++handshake_count;
            if(handshake_count>1 && wrong_recovery_echo) {failed_state=true;banner();}
            else {failed_state=false;add(src,n);}
        } else if(n==18 && src[0]==2 && src[1]==0 && src[6]==2 && src[7]==1) {
            ++window_count;
            if(window_count==fail_window) failed_state=true;
            else {const std::uint8_t b[]={2,3,7,0,0,0};add(b,6);}
        } else ++payload_writes;
    }
    void discard_input() override {rx.clear();}
};
std::vector<std::uint8_t> image(unsigned tick) {
    std::vector<std::uint8_t> out;msu2::to_rgb565(msu2::make_pattern("moving",tick),out);return out;
}
void source_test(unsigned fps) {
    for(unsigned hold:{3000,6000}) {
        msu2diag::configure(fps,hold);
        auto frozen=image(msu2diag::source_tick(fps));
        msu2::DeltaPlanner p;p.commit(frozen);
        const auto end=fps+fps*hold/1000;
        require(msu2diag::source_tick(fps-1)==fps-1,"warmup mapping");
        for(unsigned i=fps;i<end;++i) {
            auto rgb=image(msu2diag::source_tick(i));
            require(rgb==frozen && p.plan(rgb,msu2::Encoding::pair).patches.empty(),"static frame must send nothing");
        }
        require(msu2diag::source_tick(end)==fps,"resume mapping");
        require(!p.plan(image(msu2diag::source_tick(end)),msu2::Encoding::pair).patches.empty(),"resume must differ");
    }
}
void recovery_gate_test() {
    Fake fake;
    msu2diag::ObservedTransport observed(fake,false);
    observed.begin_send(1,0,1);
    const std::uint8_t command[18]={2,0,0,0,0,0,2,1,0,1,0,1,2,3,7,0,0,0};
    observed.write_all(command,18,msu2::Milliseconds(15));
    fake.rx.clear();fake.banner();
    std::uint8_t bytes[512];
    for(unsigned i=0;i<3;++i) observed.read(bytes,sizeof bytes,msu2::Milliseconds(1));
    require(!observed.claim_recovery(true,0,"transport read error"),"non-timeout read error must not trigger recovery");
    require(!observed.claim_recovery(true,0,"multi-rectangle update deadline exceeded"),"frame deadline must not trigger recovery");
    observed.done_send();
    require(!observed.claim_recovery(true,1,"response timeout (check exclusive port access, firmware and ACK mode)"),"successful patch must clear waiting phase");
    require(!observed.recovery_attempted(),"rejected causes must not consume recovery budget");
}
void recovery_test(bool enabled,bool advertise,bool wrong_echo,unsigned failing_patch,bool fixture=false) {
    Fake fake;fake.advertise=advertise;fake.wrong_recovery_echo=wrong_echo;
    msu2diag::ObservedTransport observed(fake,false);
    msu2::SessionOptions options;options.io_timeout=msu2::Milliseconds(15);
    options.handshake_timeout=msu2::Milliseconds(15);options.write_chunk=60;
    msu2::Session session(observed,options);observed.handshake_phase();
    observed.handshake_done(session.handshake());
    msu2::DeltaPlanner planner;auto first=image(0);auto plan=planner.plan(first,msu2::Encoding::pair);
    observed.plan_event(0,plan);
    msu2diag::send_plan(session,observed,planner,plan,first,msu2::Encoding::pair,options,0,msu2diag::Clock::now(),enabled);
    planner.commit(first);
    unsigned second_tick=1;
    auto second=image(second_tick);plan=planner.plan(second,msu2::Encoding::pair);
    while(plan.patches.size()<=failing_patch && second_tick<1000) {
        second=image(++second_tick);plan=planner.plan(second,msu2::Encoding::pair);
    }
    require(plan.patches.size()>failing_patch,"test must reach designated patch");
    fake.fail_window=fake.window_count+1+failing_patch;
    observed.plan_event(1,plan);const auto before=fake.payload_writes;
    bool failed=false;
    try {msu2diag::send_plan(session,observed,planner,plan,second,msu2::Encoding::pair,options,1,msu2diag::Clock::now(),enabled);}
    catch(const std::exception&){failed=true;}
    const bool expected=enabled && advertise && !wrong_echo;
    require(failed!=expected,"recovery result");
    require(!planner.has_reference(),"failure/recovery must invalidate before caller commit");
    require(fake.handshake_count==(enabled&&advertise?2u:1u),"strict gate must suppress non-MSN retries");
    std::ostringstream log;observed.dump(log);
    require(log.str().find("phase=original_failure")!=std::string::npos,"original failure missing");
    if(expected) {
        require(plan.kind==msu2::UpdateKind::full,"recovered update must be full");
        require(fake.payload_writes>before,"successful full must have payload");
        planner.commit(second);
        auto third=image(second_tick+1);plan=planner.plan(third,msu2::Encoding::pair);observed.plan_event(2,plan);
        require(plan.kind!=msu2::UpdateKind::full,"next frame resumes existing delta planner");
        fake.fail_window=fake.window_count+1;
        bool bounded=false;
        try {msu2diag::send_plan(session,observed,planner,plan,third,msu2::Encoding::pair,options,2,msu2diag::Clock::now(),true);}
        catch(const std::exception&){bounded=true;}
        require(bounded && fake.handshake_count==2,"second fault must not trigger another recovery");
    } else if(failing_patch==0) require(fake.payload_writes==before,"failed window must not send payload");
    if(fixture) observed.dump(std::cout);
}
}
int main(int argc,char** argv) {
    try {
        if(argc==2 && std::string(argv[1])=="--trace-fixture") {
            recovery_test(true,true,false,1,true);return 0;
        }
        source_test(45);source_test(60);recovery_gate_test();
        recovery_test(false,true,false,0);
        recovery_test(true,false,false,0);
        recovery_test(true,true,true,0);
        recovery_test(true,true,false,0);
        recovery_test(true,true,false,1);
        std::cout<<"Static mapping, unchanged suppression, MSN gate, strict echo, invalidation/full and one-attempt bound: PASS (fake only)\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
