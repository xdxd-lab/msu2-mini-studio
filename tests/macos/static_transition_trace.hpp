#pragma once
// Isolated diagnostic instrumentation. Native Session/Transport/DeltaPlanner
// remain unchanged. No file output or allocation on the observed I/O path.
#include "msu2/delta.hpp"
#include "msu2/session.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace msu2diag {
using Clock = std::chrono::steady_clock;
inline unsigned source_fps=0, static_frames=0;
inline void configure(unsigned fps, unsigned static_ms) {
    if ((fps!=45 && fps!=60) || (static_ms!=3000 && static_ms!=6000))
        throw std::invalid_argument("diagnostic requires 45/60 FPS and 3000/6000 static ms");
    source_fps=fps; static_frames=fps*static_ms/1000;
}
inline unsigned source_tick(unsigned tick) {
    if (!source_fps || tick<source_fps) return tick;
    if (tick<source_fps+static_frames) return source_fps-1;
    return tick-static_frames;
}

class ObservedTransport final : public msu2::Transport {
    struct Event {
        double t_ms=0;
        const char* phase="";
        char kind='?';
        unsigned frame=0, patch=0, patch_count=0;
        std::size_t size=0, captured=0, value1=0, value2=0;
        std::array<std::uint8_t,512> data{};
    };
    static constexpr std::size_t capacity=65536;
    msu2::Transport& native_;
    Clock::time_point start_=Clock::now();
    std::unique_ptr<Event[]> events_{new Event[capacity]};
    std::size_t count_=0;
    bool overflow_=false;
    const char* phase_="banner_wait";
    unsigned frame_=0, patch_=0, patches_=0;
    std::array<std::uint8_t,6> banner_tail_{};
    unsigned tail_length_=0;
    std::size_t banners_=0;
    bool attempt_used_=false, auto_dump_;

    void remember(char kind, const char* phase, const std::uint8_t* data=nullptr,
                  std::size_t size=0, std::size_t capture=0,
                  std::size_t value1=0, std::size_t value2=0) noexcept {
        if (count_==capacity) {overflow_=true;return;}
        auto& e=events_[count_++];
        e.t_ms=std::chrono::duration<double,std::milli>(Clock::now()-start_).count();
        e.kind=kind;e.phase=phase;e.frame=frame_;e.patch=patch_;e.patch_count=patches_;
        e.size=size;e.captured=std::min(capture,e.data.size());e.value1=value1;e.value2=value2;
        if(e.captured && data) std::memcpy(e.data.data(),data,e.captured);
    }
    void scan(const std::uint8_t* data, std::size_t size) noexcept {
        for(std::size_t i=0;i<size;++i) {
            if(tail_length_<6) banner_tail_[tail_length_++]=data[i];
            else {std::move(banner_tail_.begin()+1,banner_tail_.end(),banner_tail_.begin());banner_tail_[5]=data[i];}
            auto& b=banner_tail_;
            if(tail_length_==6 && b[0]==0 && b[1]=='M' && b[2]=='S' && b[3]=='N'
               && b[4]>='0' && b[4]<='9' && b[5]>='0' && b[5]<='9') ++banners_;
        }
    }
public:
    explicit ObservedTransport(msu2::Transport& t, bool auto_dump=true):native_(t),auto_dump_(auto_dump) {}
    ~ObservedTransport() override {
        if(auto_dump_) {try {dump(std::cout);}catch(...) {}}
    }
    void dump(std::ostream& out) const {
        out<<std::fixed<<std::setprecision(6);
        for(std::size_t i=0;i<count_;++i) {
            const auto& e=events_[i];
            out<<"OBS t_ms="<<e.t_ms<<" phase="<<e.phase<<" kind="<<e.kind
               <<" frame="<<e.frame<<" patch="<<e.patch<<" patch_count="<<e.patch_count
               <<" size="<<e.size<<" captured="<<e.captured<<" value1="<<e.value1<<" value2="<<e.value2<<" data=";
            static const char digits[]="0123456789abcdef";
            for(std::size_t j=0;j<e.captured;++j) out<<digits[e.data[j]>>4]<<digits[e.data[j]&15];
            out<<'\n';
        }
        out<<"TRACE events="<<count_<<" overflow="<<(overflow_?1:0)<<'\n';
    }
    void handshake_phase(bool recovery=false) {
        phase_="banner_wait";
        remember('M',recovery?"recovery_begin":"initial_handshake");
    }
    void handshake_done(int version, bool recovery=false) {
        remember('M',recovery?"recovery_handshake_ok":"initial_handshake_ok",nullptr,0,0,version);
    }
    void plan_event(unsigned frame, const msu2::UpdatePlan& p) {
        frame_=frame;patch_=0;patches_=static_cast<unsigned>(p.patches.size());
        remember('P',msu2::update_kind_name(p.kind),nullptr,0,0,p.wire_bytes,p.changed_pixels);
    }
    void begin_send(unsigned frame,unsigned patch,unsigned count) {
        frame_=frame;patch_=patch;patches_=count;
        phase_="input_discard";tail_length_=0;banners_=0;
    }
    void done_send() {phase_="patch_complete";remember('M',"patch_ok");}
    void update_done(const msu2::UpdatePlan& p,const msu2::TransferStats& stats) {
        remember('U',msu2::update_kind_name(p.kind),nullptr,0,0,stats.bytes,stats.write_calls);
    }
    bool claim_recovery(bool enabled, std::size_t completed, const char* error) {
        remember('E',phase_,reinterpret_cast<const std::uint8_t*>(error),std::strlen(error),std::strlen(error),banners_,completed);
        remember('M',"original_failure",nullptr,0,0,banners_,completed);
        if(!enabled || attempt_used_ || std::strcmp(phase_,"window_ack_wait") || banners_==0
           || std::strcmp(error,"response timeout (check exclusive port access, firmware and ACK mode)")) return false;
        attempt_used_=true;return true;
    }
    void failure(const char* error) {
        remember('E',phase_,reinterpret_cast<const std::uint8_t*>(error),std::strlen(error),std::strlen(error));
    }
    void mark(const char* phase) {remember('M',phase);}
    bool recovery_attempted() const {return attempt_used_;}
    std::size_t read(std::uint8_t* dst,std::size_t n,msu2::Milliseconds timeout) override {
        auto size=native_.read(dst,n,timeout);
        remember('R',phase_,dst,size,std::min(size,n));
        if(size<=n && !std::strcmp(phase_,"window_ack_wait")) scan(dst,size);
        return size;
    }
    void write_all(const std::uint8_t* src,std::size_t size,msu2::Milliseconds timeout) override {
        const bool echo=size==msu2::handshake_command.size()
            && std::equal(msu2::handshake_command.begin(),msu2::handshake_command.end(),src);
        if(echo) phase_="echo_write";
        else if(!std::strcmp(phase_,"input_discard")) phase_="window_write";
        else phase_="payload_write";
        remember('T',phase_,src,size,std::min<std::size_t>(size,18));
        native_.write_all(src,size,timeout);
        remember('W',phase_,src,size,std::min<std::size_t>(size,18));
        if(echo) phase_="echo_wait";
        else if(!std::strcmp(phase_,"window_write")) phase_="window_ack_wait";
    }
    void discard_input() override {
        remember('F',"input_discard");native_.discard_input();
    }
};

// Original send loop/deadline and invalidation, with one opt-in diagnostic
// recovery. A recovery cannot commit a partial reference or run without MSN.
inline msu2::TransferStats send_plan(msu2::Session& session, ObservedTransport& observed,
        msu2::DeltaPlanner& planner, msu2::UpdatePlan& plan,
        const std::vector<std::uint8_t>& rgb565, msu2::Encoding mode,
        const msu2::SessionOptions& options, unsigned frame, Clock::time_point after, bool recover) {
    msu2::TransferStats st{};
    std::size_t completed=0;
    auto send_all=[&](Clock::time_point deadline) {
        for(std::size_t i=0;i<plan.patches.size();++i) {
            if(Clock::now()>=deadline) throw std::runtime_error("multi-rectangle update deadline exceeded");
            observed.begin_send(frame,static_cast<unsigned>(i),static_cast<unsigned>(plan.patches.size()));
            auto part=session.send(plan.patches[i].frame);
            st.bytes+=part.bytes;st.write_calls+=part.write_calls;
            st.window_ack_ms+=part.window_ack_ms;st.payload_write_ms+=part.payload_write_ms;
            ++completed;observed.done_send();
        }
    };
    try {send_all(after+options.io_timeout);}
    catch(const std::exception& error) {
        planner.invalidate();
        if(!observed.claim_recovery(recover,completed,error.what())) throw;
        try {
            observed.handshake_phase(true);
            auto version=session.handshake();observed.handshake_done(version,true);
            plan=planner.plan(rgb565,mode);
            if(plan.kind!=msu2::UpdateKind::full) throw std::logic_error("invalidated planner did not force full");
            observed.plan_event(frame,plan);observed.mark("recovery_full_begin");
            st={};completed=0;send_all(Clock::now()+options.io_timeout);
            observed.mark("recovery_full_ok");
        } catch(const std::exception& recovery_error) {
            planner.invalidate();observed.failure(recovery_error.what());observed.mark("recovery_failed");throw;
        }
    } catch(...) {planner.invalidate();throw;}
    observed.update_done(plan,st);
    return st;
}
}
