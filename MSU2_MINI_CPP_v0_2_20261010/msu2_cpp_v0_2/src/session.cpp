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
}
Session::Session(Transport& t,SessionOptions o):transport_(t),options_(o),owner_(std::this_thread::get_id()) {
    if(o.handshake_timeout.count()<=0 || o.io_timeout.count()<=0 || o.write_chunk<6 || o.write_chunk%6 || o.write_chunk>65532)
        throw std::invalid_argument("timeouts must be positive; write chunk must be 6..65532 and divisible by 6");
}
void Session::require_owner() const {
    if(owner_!=std::this_thread::get_id()) throw std::logic_error("Session must be used by its owning thread");
}
void Session::wait_pattern(const std::vector<std::uint8_t>& pattern,Milliseconds timeout) {
    const auto deadline=Clock::now()+timeout;
    std::vector<std::uint8_t> pending;
    std::array<std::uint8_t,512> buf{};
    last_response_.clear();
    while(Clock::now()<deadline) {
        auto n=transport_.read(buf.data(),buf.size(),std::min(remaining(deadline),Milliseconds(50)));
        if(n>buf.size()) throw std::runtime_error("transport returned an invalid byte count");
        if(!n) continue;
        pending.insert(pending.end(),buf.begin(),buf.begin()+static_cast<std::ptrdiff_t>(n));
        auto p=std::search(pending.begin(),pending.end(),pattern.begin(),pattern.end());
        if(p!=pending.end()) {last_response_=pending;return;}
        if(pending.size()>pattern.size()-1)
            pending.erase(pending.begin(),pending.end()-static_cast<std::ptrdiff_t>(pattern.size()-1));
    }
    throw std::runtime_error("response timeout (check exclusive port access, firmware and ACK mode)");
}
int Session::handshake() {
    require_owner();connected_=false;
    // Do not flush on entry: a valid banner may already be queued.
    const auto deadline=Clock::now()+options_.handshake_timeout;
    std::vector<std::uint8_t> pending;
    std::array<std::uint8_t,512> buf{};
    int version=-1;
    while(Clock::now()<deadline && version<0) {
        auto n=transport_.read(buf.data(),buf.size(),std::min(remaining(deadline),Milliseconds(50)));
        if(n>buf.size()) throw std::runtime_error("transport returned an invalid byte count");
        if(!n) continue;
        pending.insert(pending.end(),buf.begin(),buf.begin()+static_cast<std::ptrdiff_t>(n));
        for(std::size_t i=0;i+6<=pending.size();++i) {
            auto* b=pending.data()+i;
            if(b[0]==0 && b[1]=='M' && b[2]=='S' && b[3]=='N' && b[4]>='0' && b[4]<='9' && b[5]>='0' && b[5]<='9') {
                version=(b[4]-'0')*10+b[5]-'0';break;
            }
        }
        if(pending.size()>5) pending.erase(pending.begin(),pending.end()-5);
    }
    if(version<0) throw std::runtime_error("no MSN version banner; close other screen apps and replug the MINI");
    transport_.write_all(handshake_command.data(),handshake_command.size(),options_.io_timeout);
    wait_pattern({handshake_command.begin(),handshake_command.end()},options_.io_timeout);
    connected_=true;return version;
}
void Session::await_lcd(const Command& cmd) {
    if(options_.ack_mode==AckMode::echo) wait_pattern({cmd.begin(),cmd.end()},options_.io_timeout);
    else wait_pattern({2,3},options_.io_timeout); // established legacy prefix only, not a frame-complete receipt
}
void Session::set_orientation(unsigned value) {
    require_owner();if(!connected_)throw std::logic_error("handshake required before orientation");
    auto cmd=orientation_command(value);
    try {
        transport_.discard_input();
        transport_.write_all(cmd.data(),cmd.size(),options_.io_timeout);
        await_lcd(cmd);
    } catch(...) {connected_=false;throw;}
}
TransferStats Session::send(const EncodedFrame& frame) {
    require_owner();if(!connected_)throw std::logic_error("handshake required before sending");
    if(frame.payload.empty() || frame.payload.size()%6)throw std::invalid_argument("invalid encoded payload");
    TransferStats stats;
    try {
        transport_.discard_input(); // discard completed prior traffic, never pending output
        const auto start=Clock::now();
        transport_.write_all(frame.window.data(),frame.window.size(),options_.io_timeout);
        stats.write_calls=1;
        await_lcd({2,3,7,0,0,0});
        const auto after_ack=Clock::now();
        const auto deadline=after_ack+options_.io_timeout;
        for(std::size_t pos=0;pos<frame.payload.size();) {
            auto budget=remaining(deadline);
            if(budget.count()==0)throw std::runtime_error("frame payload write deadline exceeded");
            auto n=std::min(options_.write_chunk,frame.payload.size()-pos);
            transport_.write_all(frame.payload.data()+pos,n,budget);
            pos+=n;++stats.write_calls;
        }
        stats.window_ack_ms=millis(after_ack-start);
        stats.payload_write_ms=millis(Clock::now()-after_ack);
        stats.bytes=frame.wire_bytes();
    } catch(...) {connected_=false;throw;}
    return stats;
}
}
