// Diagnostic wrapper around the unchanged v0.2 transport/session.
// Default is handshake only. Optional bars transmits one existing LCD pattern.
#include "msu2/session.hpp"
#include "msu2/image.hpp"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

class TraceTransport final : public msu2::Transport {
    msu2::SerialTransport serial;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    unsigned lines = 0;
    void dump(const char* direction, const std::uint8_t* data, std::size_t length) {
        if (++lines > 100) return;
        double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count();
        std::cerr << std::fixed << std::setprecision(3) << elapsed << " ms " << direction << " " << length << " bytes:";
        for (std::size_t i=0; i<std::min<std::size_t>(length,64); ++i)
            std::cerr << ' ' << std::hex << std::setw(2) << std::setfill('0') << unsigned(data[i]);
        std::cerr << std::dec << std::setfill(' ') << '\n';
    }
public:
    explicit TraceTransport(const char* port) : serial(port) {}
    std::size_t read(std::uint8_t* dst, std::size_t size, msu2::Milliseconds timeout) override {
        auto n=serial.read(dst,size,timeout); if(n)dump("RX",dst,n); return n;
    }
    void write_all(const std::uint8_t* src, std::size_t size, msu2::Milliseconds timeout) override {
        dump("TX",src,size); serial.write_all(src,size,timeout);
        std::cerr << "OS accepted write (not a device acknowledgement)\n";
    }
    void discard_input() override { serial.discard_input(); }
};

int main(int argc, char** argv) {
    if(argc!=2 && argc!=4 && argc!=5){std::cerr << "Usage: serial_probe /dev/cu.usbmodem... [--bars settle_ms [hold_ms]]\n";return 2;}
    try {
        int settle=0;
        int hold=250;
        if(argc>=4){
            if(std::string(argv[2])!="--bars")throw std::invalid_argument("expected --bars");
            settle=std::stoi(argv[3]);
            if(settle<0 || settle>5000)throw std::invalid_argument("settle_ms must be 0..5000");
            if(argc==5){hold=std::stoi(argv[4]);if(hold<0 || hold>60000)throw std::invalid_argument("hold_ms must be 0..60000");}
        }
        TraceTransport transport(argv[1]);
        msu2::SessionOptions options;
        options.handshake_timeout=msu2::Milliseconds(5000);
        options.io_timeout=msu2::Milliseconds(5000);
        msu2::Session session(transport,options);
        auto version=session.handshake();
        std::cout << "Handshake verified; firmware_banner=" << version << '\n';
        if(argc>=4){
            std::this_thread::sleep_for(msu2::Milliseconds(settle));
            std::vector<std::uint8_t> pixels;
            msu2::to_rgb565(msu2::make_pattern("bars"),pixels);
            msu2::EncodedFrame frame;
            msu2::encode_frame(pixels,{},msu2::Encoding::pair,frame);
            const auto stats=session.send(frame);
            std::cout << "Bars host_write_bytes=" << stats.bytes << '\n';
            std::this_thread::sleep_for(msu2::Milliseconds(hold));
        }
        return 0;
    } catch(const std::exception& error) {std::cerr << "error: " << error.what() << '\n';return 1;}
}
