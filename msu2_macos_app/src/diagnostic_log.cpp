#include "diagnostic_log.hpp"
#include <chrono>
#include <stdexcept>

namespace msu2app {
DiagnosticLog::DiagnosticLog(const std::string& path,const std::string& wall_time):out_(path){
    if(!out_)throw std::runtime_error("無法建立診斷紀錄");
    pending_.reserve(capacity);writing_.reserve(capacity);
    out_<<"t_us,kind,stage,result,frame,patch,v0,v1,v2,v3,v4,v5,rx_len,rx_hex,detail\n";
    msu2::TraceEvent begin;begin.t_ns=origin_;begin.kind="log";begin.stage="begin";msu2::trace_detail(begin,"wall="+wall_time);write(begin);
    out_.flush();if(!out_)throw std::runtime_error("無法寫入診斷紀錄");
    writer_=std::thread([this]{run();});
}
DiagnosticLog::~DiagnosticLog(){finish();}
DiagnosticLog::Status DiagnosticLog::finish(){
    {std::lock_guard<std::mutex> lock(mutex_);done_=true;}wake_.notify_all();
    if(writer_.joinable())writer_.join();
    return status();
}
DiagnosticLog::Status DiagnosticLog::status()const{std::lock_guard<std::mutex> lock(mutex_);return status_;}
void DiagnosticLog::record(const msu2::TraceEvent& e)noexcept{
    std::lock_guard<std::mutex> lock(mutex_);
    if(pending_.size()<capacity){pending_.push_back(e);return;}
    if(!lost_)first_lost_ns_=e.t_ns;
    last_lost_ns_=e.t_ns;++lost_;lost_any_.store(true,std::memory_order_relaxed);
}
void DiagnosticLog::run(){
    bool finished=false;
    while(!finished){
        std::uint64_t lost=0;std::int64_t first_lost=0,last_lost=0;
        {std::unique_lock<std::mutex> lock(mutex_);wake_.wait_for(lock,std::chrono::milliseconds(200),[&]{return done_;});
            finished=done_;writing_.swap(pending_);lost=lost_;lost_=0;first_lost=first_lost_ns_;last_lost=last_lost_ns_;}
        const auto batch=writing_.size();
        if(!failed_){
            for(const auto& e:writing_)write(e);
            if(lost){msu2::TraceEvent e;e.t_ns=msu2::trace_now_ns();e.kind="log";e.stage="lost";
                e.v={std::int64_t(lost),std::int64_t(status_lost_full()+lost),(first_lost-origin_)/1000,(last_lost-origin_)/1000,0,0};write(e);}
            out_.flush();
        }
        writing_.clear();
        // A failed batch may be partly on disk; it is still counted as lost.
        const bool now_failed=!failed_&&!out_;
        {std::lock_guard<std::mutex> lock(mutex_);status_.lost_full+=lost;
            if(failed_||now_failed)status_.lost_after_failure+=batch;else status_.written+=batch;
            if(now_failed){status_.write_failed=true;status_.failed_at_us=(msu2::trace_now_ns()-origin_)/1000;}}
        if(now_failed)failed_.store(true,std::memory_order_relaxed);
    }
    if(failed_)return;
    const auto st=status();
    msu2::TraceEvent end;end.t_ns=msu2::trace_now_ns();end.kind="log";end.stage="end";
    end.v={std::int64_t(st.written),std::int64_t(st.lost_full),std::int64_t(st.lost_after_failure),0,0,0};write(end);out_.flush();
    if(!out_){std::lock_guard<std::mutex> lock(mutex_);status_.write_failed=true;status_.failed_at_us=(msu2::trace_now_ns()-origin_)/1000;failed_.store(true,std::memory_order_relaxed);}
}
std::uint64_t DiagnosticLog::status_lost_full()const{std::lock_guard<std::mutex> lock(mutex_);return status_.lost_full;}
void DiagnosticLog::write(const msu2::TraceEvent& e){
    static const char hex[]="0123456789abcdef";
    out_<<(e.t_ns-origin_)/1000<<','<<e.kind<<','<<e.stage<<','<<e.result<<','<<e.frame<<','<<e.patch;
    for(auto v:e.v)out_<<','<<v;
    out_<<','<<unsigned(e.rx_len)<<',';
    for(std::size_t i=0;i<e.rx_len&&i<e.rx.size();++i)out_<<hex[e.rx[i]>>4]<<hex[e.rx[i]&15];
    out_<<",\"";
    for(const char* c=e.detail.data();*c;++c)out_<<(*c=='"'?'\'':*c=='\n'||*c=='\r'?' ':*c);
    out_<<"\"\n";
}
}
