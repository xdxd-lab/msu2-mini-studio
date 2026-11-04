#include "msu2/session.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>

namespace msu2 {
namespace {
using Clock=std::chrono::steady_clock;
Milliseconds remaining(Clock::time_point deadline) {
    const auto delta=deadline-Clock::now();
    if(delta<=Clock::duration::zero()) return Milliseconds(0);
    return std::max(Milliseconds(1),std::chrono::duration_cast<Milliseconds>(delta));
}
double millis(Clock::duration d) {return std::chrono::duration<double,std::milli>(d).count();}
// Received-byte counts for one wait; only filled while tracing. Raw bytes are
// kept only for the pattern the caller's acceptance rule matched.
// The classifier counts occurrences of three known patterns in the bytes the
// wait already read, across read boundaries, using only the last 6 bytes. It
// is not an acceptance rule and never changes what the wait accepts.
struct RxTrace {
    static constexpr std::size_t max_reads=32;
    Clock::time_point begin=Clock::now();
    std::size_t total=0, reads=0, trimmed=0;
    std::int64_t first_byte_us=-1;
    std::uint64_t last6=0; // last 6 received bytes, oldest in bits 47..40
    std::int64_t banners=0, echoes=0, prefixes=0;
    std::array<std::int64_t,max_reads> read_us; // filled up to min(reads,max_reads)
    std::array<std::size_t,max_reads> read_len;
    void add(const std::uint8_t* data,std::size_t n) {
        const auto t=trace_us(Clock::now()-begin);
        if(first_byte_us<0) first_byte_us=t;
        if(reads<max_reads) {read_us[reads]=t; read_len[reads]=n;}
        for(std::size_t i=0;i<n;++i) {
            last6=((last6<<8)|data[i])&0xffffffffffffULL;
            const auto seen=total+i+1;
            if(seen>=2 && (last6&0xffff)==0x0203) ++prefixes;
            if(seen<6) continue;
            const auto d1=std::uint8_t(last6>>8), d0=std::uint8_t(last6);
            if((last6>>16)==0x004d534eULL && d1>='0' && d1<='9' && d0>='0' && d0<='9') ++banners;
            else if(last6==0x004d534e434eULL) ++echoes;
        }
        ++reads; total+=n;
    }
};
// Follows the stage's rx row. Per-read rows are omitted for a wait that
// matched in its only read, since rx first_byte_us and rx_total equal them.
void record_rx_class(TraceSink& sink,TraceEvent e,const RxTrace& rx,bool matched) {
    const auto recorded=std::min(rx.reads,RxTrace::max_reads);
    e.kind="rxclass";
    e.v={std::int64_t(rx.total),rx.banners,rx.echoes,rx.prefixes,std::int64_t(recorded),std::int64_t(rx.reads-recorded)};
    sink.record(e);
    if(matched && rx.reads<=1) return;
    e.kind="rxread"; e.result="read";
    for(std::size_t i=0;i<recorded;++i) {
        e.v={std::int64_t(i),rx.read_us[i],std::int64_t(rx.read_len[i]),0,0,0};
        sink.record(e);
    }
}
}
Session::Session(Transport& t,SessionOptions o):transport_(t),options_(o),owner_(std::this_thread::get_id()) {
    if(o.handshake_timeout.count()<=0 || o.io_timeout.count()<=0 || o.write_chunk<6 || o.write_chunk%6 || o.write_chunk>65532)
        throw std::invalid_argument("timeouts must be positive; write chunk must be 6..65532 and divisible by 6");
}
void Session::require_owner() const {
    if(owner_!=std::this_thread::get_id()) throw std::logic_error("Session must be used by its owning thread");
}
TraceEvent Session::trace_event(const char* kind,const char* stage,const char* result) const {
    TraceEvent e; e.t_ns=trace_now_ns(); e.kind=kind; e.stage=stage; e.result=result;
    e.frame=trace_frame_; e.patch=trace_patch_; return e;
}
void Session::trace_tx(const char* stage,std::size_t length,std::int64_t write_us,std::size_t offset,std::size_t index) const {
    auto e=trace_event("tx",stage,"ok");
    e.v={std::int64_t(length),write_us,std::int64_t(offset),std::int64_t(index),0,0};
    options_.trace->record(e);
}
// Called from a catch block: records which I/O step raised and its message.
void Session::trace_failure(const char* stage,std::size_t done,std::int64_t elapsed_us) const {
    if(!options_.trace) return;
    auto e=trace_event("fail",stage,"exception");
    e.v[0]=std::int64_t(done); e.v[1]=elapsed_us;
    try {throw;} catch(const std::exception& error) {trace_detail(e,error.what());} catch(...) {trace_detail(e,"unknown exception");}
    options_.trace->record(e);
}
void Session::wait_pattern(const std::vector<std::uint8_t>& pattern,Milliseconds timeout,const char* stage) {
    const auto deadline=Clock::now()+timeout;
    std::vector<std::uint8_t> pending;
    std::array<std::uint8_t,512> buf{};
    RxTrace rx;
    last_response_.clear();
    const auto report=[&](const char* result,std::int64_t match_offset) {
        if(!options_.trace) return;
        auto e=trace_event("rx",stage,result);
        const auto trailing=match_offset<0?0:std::int64_t(rx.total)-match_offset-std::int64_t(pattern.size());
        e.v={trace_us(Clock::now()-rx.begin),std::int64_t(rx.total),std::int64_t(rx.reads),rx.first_byte_us,match_offset,trailing};
        if(match_offset>=0) {
            e.rx_len=std::uint8_t(std::min(pattern.size(),e.rx.size()));
            std::copy_n(pattern.begin(),e.rx_len,e.rx.begin());
        }
        options_.trace->record(e);
        e.rx_len=0;
        record_rx_class(*options_.trace,e,rx,match_offset>=0);
    };
    while(Clock::now()<deadline) {
        auto n=transport_.read(buf.data(),buf.size(),std::min(remaining(deadline),Milliseconds(50)));
        if(n>buf.size()) throw std::runtime_error("transport returned an invalid byte count");
        if(!n) continue;
        if(options_.trace) rx.add(buf.data(),n);
        pending.insert(pending.end(),buf.begin(),buf.begin()+static_cast<std::ptrdiff_t>(n));
        auto p=std::search(pending.begin(),pending.end(),pattern.begin(),pattern.end());
        if(p!=pending.end()) {last_response_=pending;report("ok",std::int64_t(rx.trimmed)+(p-pending.begin()));return;}
        if(pending.size()>pattern.size()-1) {
            rx.trimmed+=pending.size()-(pattern.size()-1);
            pending.erase(pending.begin(),pending.end()-static_cast<std::ptrdiff_t>(pattern.size()-1));
        }
    }
    report("timeout",-1);
    throw std::runtime_error("response timeout (check exclusive port access, firmware and ACK mode)");
}
int Session::handshake() {
    require_owner();connected_=false;
    const char* stage="handshake_banner";
    auto stage_start=Clock::now();
    try {
        // Do not flush on entry: a valid banner may already be queued.
        const auto deadline=Clock::now()+options_.handshake_timeout;
        std::vector<std::uint8_t> pending;
        std::array<std::uint8_t,512> buf{};
        RxTrace rx;
        int version=-1;
        std::int64_t match_offset=-1;
        while(Clock::now()<deadline && version<0) {
            auto n=transport_.read(buf.data(),buf.size(),std::min(remaining(deadline),Milliseconds(50)));
            if(n>buf.size()) throw std::runtime_error("transport returned an invalid byte count");
            if(!n) continue;
            if(options_.trace) rx.add(buf.data(),n);
            pending.insert(pending.end(),buf.begin(),buf.begin()+static_cast<std::ptrdiff_t>(n));
            for(std::size_t i=0;i+6<=pending.size();++i) {
                auto* b=pending.data()+i;
                if(b[0]==0 && b[1]=='M' && b[2]=='S' && b[3]=='N' && b[4]>='0' && b[4]<='9' && b[5]>='0' && b[5]<='9') {
                    version=(b[4]-'0')*10+b[5]-'0';match_offset=std::int64_t(rx.trimmed+i);break;
                }
            }
            if(pending.size()>5) {rx.trimmed+=pending.size()-5;pending.erase(pending.begin(),pending.end()-5);}
        }
        if(options_.trace) {
            auto e=trace_event("rx",stage,version<0?"timeout":"ok");
            e.v={trace_us(Clock::now()-rx.begin),std::int64_t(rx.total),std::int64_t(rx.reads),rx.first_byte_us,version,match_offset};
            if(version>=0) {e.rx={0,'M','S','N',std::uint8_t('0'+version/10),std::uint8_t('0'+version%10)};e.rx_len=6;}
            options_.trace->record(e);
            e.rx_len=0;
            record_rx_class(*options_.trace,e,rx,version>=0);
        }
        if(version<0) throw std::runtime_error("no MSN version banner; close other screen apps and replug the MINI");
        stage="handshake"; stage_start=Clock::now();
        transport_.write_all(handshake_command.data(),handshake_command.size(),options_.io_timeout);
        if(options_.trace) trace_tx(stage,handshake_command.size(),trace_us(Clock::now()-stage_start));
        stage="handshake_echo"; stage_start=Clock::now();
        wait_pattern({handshake_command.begin(),handshake_command.end()},options_.io_timeout,stage);
        connected_=true;return version;
    } catch(...) {trace_failure(stage,0,trace_us(Clock::now()-stage_start));throw;}
}
void Session::await_lcd(const Command& cmd,const char* stage) {
    if(options_.ack_mode==AckMode::echo) wait_pattern({cmd.begin(),cmd.end()},options_.io_timeout,stage);
    else wait_pattern({2,3},options_.io_timeout,stage); // established legacy prefix only, not a frame-complete receipt
}
void Session::set_orientation(unsigned value) {
    require_owner();if(!connected_)throw std::logic_error("handshake required before orientation");
    auto cmd=orientation_command(value);
    const char* stage="orientation";
    auto stage_start=Clock::now();
    try {
        transport_.discard_input();
        transport_.write_all(cmd.data(),cmd.size(),options_.io_timeout);
        if(options_.trace) trace_tx(stage,cmd.size(),trace_us(Clock::now()-stage_start));
        stage="orientation_ack"; stage_start=Clock::now();
        await_lcd(cmd,stage);
    } catch(...) {connected_=false;trace_failure(stage,0,trace_us(Clock::now()-stage_start));throw;}
}
TransferStats Session::send(const EncodedFrame& frame) {
    require_owner();if(!connected_)throw std::logic_error("handshake required before sending");
    if(frame.payload.empty() || frame.payload.size()%6)throw std::invalid_argument("invalid encoded payload");
    TransferStats stats;
    const char* stage="discard_input";
    auto stage_start=Clock::now();
    std::size_t pos=0;
    try {
        transport_.discard_input(); // discard completed prior traffic, never pending output
        if(options_.trace) {
            auto e=trace_event("io",stage,"bytes_unavailable");
            e.v[0]=trace_us(Clock::now()-stage_start);
            options_.trace->record(e);
        }
        const auto start=Clock::now();
        stage="window_write"; stage_start=start;
        transport_.write_all(frame.window.data(),frame.window.size(),options_.io_timeout);
        if(options_.trace) trace_tx(stage,frame.window.size(),trace_us(Clock::now()-start));
        stats.write_calls=1;
        stage="window_ack"; stage_start=Clock::now();
        await_lcd({2,3,7,0,0,0},stage);
        const auto after_ack=Clock::now();
        const auto deadline=after_ack+options_.io_timeout;
        stage="payload_write"; stage_start=after_ack;
        std::int64_t max_chunk_us=0; std::size_t max_chunk_index=0;
        for(;pos<frame.payload.size();) {
            auto budget=remaining(deadline);
            if(budget.count()==0)throw std::runtime_error("frame payload write deadline exceeded");
            auto n=std::min(options_.write_chunk,frame.payload.size()-pos);
            const auto chunk_start=options_.trace?Clock::now():Clock::time_point{};
            transport_.write_all(frame.payload.data()+pos,n,budget);
            if(options_.trace) {
                const auto chunk_us=trace_us(Clock::now()-chunk_start);
                const auto index=stats.write_calls-1;
                if(chunk_us>max_chunk_us) {max_chunk_us=chunk_us;max_chunk_index=index;}
                if(options_.trace_chunks) trace_tx("payload_chunk",n,chunk_us,pos,index);
            }
            pos+=n;++stats.write_calls;
        }
        stats.window_ack_ms=millis(after_ack-start);
        stats.payload_write_ms=millis(Clock::now()-after_ack);
        stats.bytes=frame.wire_bytes();
        if(options_.trace) {
            auto e=trace_event("payload","write","ok");
            e.v={std::int64_t(frame.payload.size()),trace_us(Clock::now()-after_ack),std::int64_t(stats.write_calls-1),
                 max_chunk_us,std::int64_t(max_chunk_index),0};
            options_.trace->record(e);
        }
    } catch(...) {
        connected_=false;
        trace_failure(stage,std::string_view(stage)=="payload_write"?pos:0,trace_us(Clock::now()-stage_start));
        throw;
    }
    return stats;
}
}
