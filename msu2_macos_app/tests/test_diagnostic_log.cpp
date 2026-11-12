// Deterministic offline writer failures and bounded queue; no disk exhaustion.
#include "../src/diagnostic_log.hpp"
#include <condition_variable>
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace std::chrono_literals;
namespace {
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
class Output final:public std::stringbuf {
public:
    enum class Mode { normal,write_failure,flush_failure,block_flush };
    void arm(Mode mode){std::lock_guard<std::mutex> lock(mutex);mode_=mode;}
    bool wait_blocked(){std::unique_lock<std::mutex> lock(mutex);return wake.wait_for(lock,5s,[&]{return blocked;});}
    void release(){std::lock_guard<std::mutex> lock(mutex);released=true;wake.notify_all();}
protected:
    std::streamsize xsputn(const char* s,std::streamsize n)override{
        std::lock_guard<std::mutex> lock(mutex);
        if(mode_==Mode::write_failure)return 0;
        return std::stringbuf::xsputn(s,n);
    }
    int sync()override{
        std::unique_lock<std::mutex> lock(mutex);
        if(mode_==Mode::flush_failure)return -1;
        if(mode_==Mode::block_flush){blocked=true;wake.notify_all();wake.wait(lock,[&]{return released;});}
        return 0;
    }
private:
    std::mutex mutex;std::condition_variable wake;Mode mode_=Mode::normal;bool blocked=false,released=false;
};
msu2::TraceEvent event(){msu2::TraceEvent e;e.t_ns=msu2::trace_now_ns();e.kind="frame";e.stage="done";e.result="sent";return e;}
void failure(Output::Mode mode){
    Output buffer;std::ostream output(&buffer);msu2app::DiagnosticLog log(output,"offline");
    buffer.arm(mode);log.record(event());
    const auto deadline=std::chrono::steady_clock::now()+5s;
    while(!log.write_failed()&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(1ms);
    check(log.write_failed(),"writer did not report injected failure");
    log.record(event());auto st=log.finish();
    check(st.write_failed&&st.failed_at_us>=0,"failure status missing");
    check(st.written==0&&st.lost_full==0&&st.lost_after_failure==2,"failed batch accounting");
    check(buffer.str().find(",log,end,")==std::string::npos,"failed output claimed complete");
    check(log.finish().lost_after_failure==2,"finish not idempotent");
}
void overflow(){
    Output buffer;std::ostream output(&buffer);msu2app::DiagnosticLog log(output,"offline");
    buffer.arm(Output::Mode::block_flush);log.record(event());
    // Always release the writer before throwing: even a failed test must terminate.
    if(!buffer.wait_blocked()){buffer.release();throw std::runtime_error("writer gate timed out");}
    for(unsigned i=0;i<16384+7;++i)log.record(event());
    const bool observed=log.lost_any();buffer.release();auto st=log.finish();
    check(observed,"overflow not visible to controller polling");
    check(st.written==16385&&st.lost_full==7&&st.lost_after_failure==0&&!st.write_failed,"overflow accounting");
    const auto text=buffer.str();
    check(text.find(",log,lost,,-1,-1,7,7,")!=std::string::npos,"loss marker missing");
    check(text.find(",log,end,,-1,-1,16385,7,0,")!=std::string::npos,"end counts missing");
    std::istringstream rows(text);std::string line;unsigned frames=0,lost=0,end=0;
    while(std::getline(rows,line)){if(line.find(",frame,done,")!=std::string::npos)++frames;if(line.find(",log,lost,")!=std::string::npos)++lost;if(line.find(",log,end,")!=std::string::npos)++end;}
    check(frames==16385&&lost==1&&end==1,"serialized row accounting");
}
void startup_failure(){
    for(auto mode:{Output::Mode::write_failure,Output::Mode::flush_failure}){
        Output buffer;std::ostream output(&buffer);buffer.arm(mode);bool threw=false;
        try{msu2app::DiagnosticLog log(output,"offline");}catch(const std::runtime_error&){threw=true;}
        check(threw,"startup output failure accepted");
    }
}
}
int main(){try{startup_failure();failure(Output::Mode::write_failure);failure(Output::Mode::flush_failure);overflow();std::cout<<"DiagnosticLog deterministic failure/overflow tests passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
