// Same-connection application-idle comparisons using the unchanged Session.
// Only established LCD and handshake commands; no power/firmware commands.
#include "msu2/image.hpp"
#include "msu2/session.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
static double millis(Clock::duration value) {
    return std::chrono::duration<double, std::milli>(value).count();
}

// Capture at the Transport-call level. No disk/console output during send.
class ObservedTransport final : public msu2::Transport {
    msu2::Transport& base;
    struct Event {
        double at_ms;
        std::string phase;
        char direction;
        std::size_t size;
        std::array<std::uint8_t, 6> data{};
    };
    Clock::time_point start = Clock::now();
    std::vector<Event> events;
    bool first_send_write = false;
    void record(char direction, const std::uint8_t* data, std::size_t size) {
        Event e{millis(Clock::now()-start), phase, direction, size, {}};
        if (data) std::copy_n(data, std::min(size, e.data.size()), e.data.begin());
        events.push_back(std::move(e));
    }
public:
    std::string phase = "banner_wait";
    explicit ObservedTransport(msu2::Transport& transport) : base(transport) { events.reserve(12000); }
    void begin_send() { phase="input_discard"; first_send_write=true; }
    std::size_t read(std::uint8_t* dst, std::size_t capacity, msu2::Milliseconds timeout) override {
        const auto n=base.read(dst,capacity,timeout); record('R',dst,n); return n;
    }
    void write_all(const std::uint8_t* src, std::size_t size, msu2::Milliseconds timeout) override {
        if(first_send_write) { phase="window_write"; first_send_write=false; }
        else if(phase=="window_ack_wait" || phase=="payload_write") phase="payload_write";
        else phase="handshake_echo_write";
        record('T',src,size);
        base.write_all(src,size,timeout);
        record('W',src,size);
        if(phase=="window_write") phase="window_ack_wait";
        else if(phase=="handshake_echo_write") phase="handshake_echo_wait";
    }
    void discard_input() override { base.discard_input(); record('F',nullptr,0); }
    void dump() const {
        for(const auto& e:events) {
            std::cerr << std::fixed << std::setprecision(3) << "OBS " << e.at_ms << "ms "
                      << e.phase << ' ' << e.direction << " size=" << e.size << " data=";
            for(auto b:e.data) std::cerr << std::hex << std::setw(2) << std::setfill('0') << unsigned(b);
            std::cerr << std::dec << std::setfill(' ') << '\n';
        }
    }
};

static msu2::EncodedFrame fixed_frame() {
    std::vector<std::uint8_t> pixels;
    msu2::to_rgb565(msu2::make_pattern("moving",0),pixels);
    msu2::EncodedFrame frame;
    msu2::encode_frame(pixels,{},msu2::Encoding::pair,frame);
    return frame;
}

class FakeTransport final : public msu2::Transport {
public:
    std::vector<std::uint8_t> pending{0,'M','S','N','0','1'};
    std::vector<std::uint8_t> written;
    bool ack=true, fail_payload=false;
    std::size_t read(std::uint8_t* dst,std::size_t size,msu2::Milliseconds) override {
        auto n=std::min(size,pending.size()); std::copy_n(pending.begin(),n,dst);
        pending.erase(pending.begin(),pending.begin()+n); return n;
    }
    void write_all(const std::uint8_t* src,std::size_t size,msu2::Milliseconds) override {
        if(size!=6 && size!=18 && fail_payload) throw std::runtime_error("fake payload failure");
        written.insert(written.end(),src,src+size);
        if(size==6 && std::equal(src,src+size,msu2::handshake_command.begin())) pending.assign(src,src+size);
        if(size==18 && ack) pending={2,3,7,0,0,0};
    }
    void discard_input() override { pending.clear(); }
};

static int self_test() {
    const auto frame=fixed_frame();
    if(frame.payload.size()!=32952) throw std::runtime_error("unexpected full pattern payload");
    FakeTransport fake; ObservedTransport observed(fake);
    msu2::SessionOptions options; options.write_chunk=60;
    options.handshake_timeout=options.io_timeout=msu2::Milliseconds(5);
    msu2::Session session(observed,options);
    if(session.handshake()!=1) throw std::runtime_error("handshake self-test");
    const auto prefix=fake.written.size();
    for(int i=0;i<2;++i) {
        observed.begin_send(); const auto stats=session.send(frame);
        if(stats.bytes!=32970 || stats.write_calls!=551) throw std::runtime_error("full stats self-test");
    }
    const auto size=frame.wire_bytes();
    if(!std::equal(fake.written.begin()+prefix,fake.written.begin()+prefix+size,
                   fake.written.begin()+prefix+size)) throw std::runtime_error("repeated payload differs");
    fake.ack=false; observed.begin_send();
    try {session.send(frame); throw std::logic_error("missing ACK accepted");}
    catch(const std::runtime_error&) {
        if(observed.phase!="window_ack_wait") throw std::logic_error("wrong ACK failure phase");
    }
    FakeTransport fake2; ObservedTransport observed2(fake2); msu2::Session session2(observed2,options);
    session2.handshake(); fake2.fail_payload=true; observed2.begin_send();
    try {session2.send(frame); throw std::logic_error("payload failure accepted");}
    catch(const std::runtime_error&) {
        if(observed2.phase!="payload_write") throw std::logic_error("wrong payload failure phase");
    }
    std::cout << "PASS identical full bytes, handshake, chunk/stats, ACK/payload failure attribution; fake transport only\n";
    return 0;
}

int main(int argc,char** argv) {
    if(argc==2 && std::string(argv[1])=="--self-test") return self_test();
    if(argc!=5 || std::string(argv[1])!="/dev/cu.usbmodem01234567891" ||
       (std::string(argv[3])!="ascending" && std::string(argv[3])!="descending") ||
       (std::string(argv[4])!="60" && std::string(argv[4])!="3840")) {
        std::cerr << "Usage: idle_full_probe KNOWN_PORT NEW_CSV ascending|descending 60|3840\n";return 2;
    }
    std::ifstream existing(argv[2]);
    if(existing.good()) {std::cerr << "CSV already exists\n";return 2;}
    const auto frame=fixed_frame();
    std::ofstream csv(argv[2]);
    if(!csv) {std::cerr << "cannot create CSV\n";return 2;}
    csv << "index,label,idle_requested_ms,idle_actual_ms,status,window_ack_ms,payload_write_ms,send_host_ms,wire_bytes,write_calls,failure_phase\n";
    csv << std::fixed << std::setprecision(6);
    try {
        const auto opening=Clock::now(); msu2::SerialTransport serial(argv[1]);
        std::cout << "OPEN_MS=" << millis(Clock::now()-opening) << '\n';
        ObservedTransport observed(serial);
        msu2::SessionOptions options; options.write_chunk=std::stoul(argv[4]);
        options.handshake_timeout=options.io_timeout=msu2::Milliseconds(2000);
        msu2::Session session(observed,options);
        auto before=Clock::now();
        try {
            auto version=session.handshake();
            std::cout << "MSN handshake verified; version=" << version << "; HANDSHAKE_MS=" << millis(Clock::now()-before) << '\n';
        } catch(const std::exception& e) {
            std::cerr << "FAIL phase=" << observed.phase << " handshake_ms=" << millis(Clock::now()-before) << " error=" << e.what() << '\n';
            observed.dump();return 1;
        }
        std::vector<int> delays{0,0,5000,0,30000,0,60000,0};
        if(std::string(argv[3])=="descending") delays={0,0,60000,0,30000,0,5000,0};
        // Inter-send idle is application silence; this does not prove USB bus suspend.
        auto last_done=Clock::now();
        for(std::size_t i=0;i<delays.size();++i) {
            std::cout << "BEGIN index=" << i << " idle_ms=" << delays[i] << std::endl;
            std::this_thread::sleep_for(msu2::Milliseconds(delays[i]));
            auto began=Clock::now();const auto actual=millis(began-last_done);
            observed.begin_send();
            try {
                const auto stats=session.send(frame);last_done=Clock::now();
                csv << i << ',' << (i==0?"first":(delays[i]?"after_idle":"immediate"))
                    << ',' << delays[i] << ',' << actual << ",completed," << stats.window_ack_ms
                    << ',' << stats.payload_write_ms << ',' << millis(last_done-began)
                    << ',' << stats.bytes << ',' << stats.write_calls << ",\n";
                csv.flush();std::cout << "END index=" << i << " completed\n";
            } catch(const std::exception& e) {
                csv << i << ",failed," << delays[i] << ',' << actual << ",failed,,,"
                    << millis(Clock::now()-began) << ",,," << observed.phase << '\n';csv.flush();
                std::cerr << "FAIL index=" << i << " phase=" << observed.phase << " error=" << e.what() << '\n';
                observed.dump();return 1;
            }
        }
        observed.dump();std::cout << "COMPLETE sends=8; LCD completion and power state not measured\n";
        return 0;
    } catch(const std::exception& e) {std::cerr << "error: " << e.what() << '\n';return 1;}
}
