// Independent checks of documented MSU2 wire semantics; no legacy executable oracle.
#include "msu2/image.hpp"
#include "msu2/latest.hpp"
#include "msu2/protocol.hpp"
#include "msu2/session.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
int checks = 0;
void check(bool yes, const std::string& message) {
    ++checks;
    if (!yes) throw std::runtime_error(message);
}
template<class F> void rejects(F&& f, const std::string& message) {
    bool threw=false;
    try { f(); } catch (const std::exception&) { threw=true; }
    check(threw,message);
}
template<class Exception, class F> void rejects_as(F&& f, const std::string& message) {
    bool right=false;
    try { f(); } catch (const Exception&) { right=true; }
    check(right,message);
}
std::uint16_t be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((unsigned(p[0]) << 8) | p[1]);
}
struct Decoded {
    Bytes pixels;
    std::vector<std::size_t> page_sizes;
    std::size_t preset_count=0;
    std::size_t stage_count=0;
};
// A device-like decoder, written from the command spec rather than encoder logic.
// 02/04 fills all 64 four-byte registers; 04/index replaces one register;
// 02/03/08 emits the indicated prefix, in wire (big-endian RGB565) byte order.
Decoded decode(const msu2::EncodedFrame& frame, msu2::Rect expected) {
    const auto& w=frame.window;
    check(w[0]==2 && w[1]==0,"XY command opcode");
    check(be16(w.data()+2)==expected.x && be16(w.data()+4)==expected.y,"XY byte order");
    check(w[6]==2 && w[7]==1,"size command opcode");
    check(be16(w.data()+8)==expected.width && be16(w.data()+10)==expected.height,"size byte order");
    check(std::equal(w.begin()+12,w.end(),msu2::Command{2,3,7,0,0,0}.begin()),"start command golden");
    check(frame.payload.size()%6==0,"payload command alignment");
    Decoded out;
    std::array<std::uint8_t,256> ram{};
    std::array<bool,64> set{};
    std::size_t page_commands=0;
    for (std::size_t pos=0;pos<frame.payload.size();pos+=6) {
        const auto* c=frame.payload.data()+pos;
        ++page_commands;
        if (c[0]==4) {
            check(c[1]<64,"stage register bounds");
            std::copy(c+2,c+6,ram.begin()+c[1]*4);
            set[c[1]]=true; ++out.stage_count;
        } else if (c[0]==2 && c[1]==4) {
            for (std::size_t i=0;i<64;++i) std::copy(c+2,c+6,ram.begin()+i*4);
            set.fill(true); ++out.preset_count;
        } else if (c[0]==2 && c[1]==3 && c[2]==8) {
            const auto length=be16(c+3);
            check(length>0 && length<=256 && length%2==0 && c[5]==0,"commit length and reserved byte");
            check(std::all_of(set.begin(),set.end(),[](bool b){return b;}),"page RAM explicitly initialized");
            if (length<256) {
                check(page_commands==65,"tail is exactly 64 stage commands plus commit");
                check(std::all_of(ram.begin()+length,ram.end(),[](std::uint8_t b){return b==255;}),"tail padding is FF");
            }
            out.pixels.insert(out.pixels.end(),ram.begin(),ram.begin()+length);
            out.page_sizes.push_back(page_commands*6);
            set.fill(false); page_commands=0;
        } else {
            throw std::runtime_error("unexpected command (including forbidden 09 tail command)");
        }
    }
    check(page_commands==0,"payload ends at a page commit");
    check(out.pixels.size()==std::size_t(expected.width)*std::size_t(expected.height)*2,"decoded rectangle size");
    return out;
}
Bytes random_bytes(std::size_t count, unsigned seed=0x4D535532u) {
    std::mt19937 rng(seed); Bytes out(count);
    for (auto& b:out) b=static_cast<std::uint8_t>(rng());
    return out;
}
void protocol_tests() {
    msu2::EncodedFrame f;
    const Bytes random=random_bytes(25600);
    msu2::encode_frame(random,{},msu2::Encoding::raw,f);
    check(f.raw_bytes==25600 && f.pages==100 && f.wire_bytes()==39018,"raw full-screen golden 39018B");
    auto d=decode(f,{});
    check(d.pixels==random,"raw random pixels reconstruct exactly");
    check(d.preset_count==0 && d.stage_count==6400,"raw stages every register");
    check(std::all_of(d.page_sizes.begin(),d.page_sizes.end(),[](std::size_t n){return n==390;}),"raw page golden 390B");
    Bytes solid(25600);
    for (std::size_t i=0;i<solid.size();i+=2) { solid[i]=0xF8; solid[i+1]=0; }
    msu2::encode_frame(solid,{},msu2::Encoding::pair,f);
    check(f.wire_bytes()==1218 && f.payload.size()==1200,"solid pair golden 1218B");
    d=decode(f,{});
    check(d.pixels==solid && d.preset_count==100 && d.stage_count==0,"solid pair exact decode");
    check(std::all_of(d.page_sizes.begin(),d.page_sizes.end(),[](std::size_t n){return n==12;}),"solid page 12B");
    msu2::encode_frame(random,{},msu2::Encoding::pair,f);
    d=decode(f,{});
    check(d.pixels==random,"pair random reconstruct exactly, no tie-choice oracle");
    check(std::all_of(d.page_sizes.begin(),d.page_sizes.end(),[](std::size_t n){return n>=12 && n<=390;}),"pair page size bounds");
    Bytes unique(256);
    for (unsigned i=0;i<64;++i) {
        unique[i*4]=0xA5; unique[i*4+1]=std::uint8_t(i); unique[i*4+2]=0x5A; unique[i*4+3]=std::uint8_t(63-i);
    }
    msu2::Rect one_page{7,9,128,1};
    msu2::encode_frame(unique,one_page,msu2::Encoding::pair,f);
    check(f.payload.size()==390 && f.pages==1,"64 unique group worst case 390B/page");
    check(decode(f,one_page).pixels==unique,"all-unique tie reconstruction");
    // Two equally frequent 32-bit groups: either preset is legal.
    Bytes tied(256);
    for (unsigned i=0;i<64;++i) std::fill(tied.begin()+i*4,tied.begin()+i*4+4,(i%2)?0xC7:0x19);
    msu2::encode_frame(tied,one_page,msu2::Encoding::pair,f);
    check(f.payload.size()==204,"32/32 tie wire size");
    check(decode(f,one_page).pixels==tied,"32/32 tie reconstructs");
    for (auto encoding:{msu2::Encoding::raw,msu2::Encoding::pair}) {
        for (int pixels:{1,3,127,128,129,255}) {
            const msu2::Rect rect=(pixels<=160)?msu2::Rect{0,0,pixels,1}:msu2::Rect{0,0,85,3};
            // Exercise any even byte tail, including 2 mod 4 and full-page plus 2B.
            const std::size_t bytes=std::size_t(pixels)*2;
            const auto source=random_bytes(bytes,unsigned(pixels));
            msu2::encode_frame(source,rect,encoding,f);
            check(decode(f,rect).pixels==source,"tail/full boundary exact reconstruction "+std::to_string(pixels));
            check(f.pages==(bytes+255)/256 && f.raw_bytes==bytes,"tail metadata");
        }
    }
    msu2::encode_frame(Bytes{0x12,0x34},{159,79,1,1},msu2::Encoding::pair,f);
    check(f.wire_bytes()==408 && decode(f,{159,79,1,1}).pixels==Bytes({0x12,0x34}),"bottom-right 1pixel tail and endian");
    for (auto rect:{msu2::Rect{-1,0,1,1},msu2::Rect{0,-1,1,1},msu2::Rect{0,0,0,1},
        msu2::Rect{0,0,1,0},msu2::Rect{0,0,-1,1},msu2::Rect{160,0,1,1},
        msu2::Rect{0,80,1,1},msu2::Rect{159,0,2,1},msu2::Rect{0,79,1,2},
        msu2::Rect{INT_MAX,0,INT_MAX,1},msu2::Rect{0,0,INT_MAX,INT_MAX}}) {
        rejects([&]{msu2::validate_rect(rect);},"invalid rectangle rejected");
        rejects([&]{msu2::encode_frame({},rect,msu2::Encoding::raw,f);},"encoder invalid rectangle rejected");
    }
    for (const auto& wrong:{Bytes{},Bytes{0},Bytes{1,2,3},Bytes{1,2,3,4}})
        rejects([&]{msu2::encode_frame(wrong,{0,0,1,1},msu2::Encoding::pair,f);},"wrong pixel-buffer size rejected");
    check(msu2::orientation_command(0)==msu2::Command({2,3,10,0,0,0}),"orientation 0 golden");
    check(msu2::orientation_command(1)==msu2::Command({2,3,10,1,0,0}),"orientation 1 golden");
    rejects([]{msu2::orientation_command(2);},"invalid orientation rejected");
    rejects([]{msu2::orientation_command(std::numeric_limits<unsigned>::max());},"large orientation rejected");
    // Reuse must clear a previous, larger payload rather than append to it.
    msu2::encode_frame(solid,{},msu2::Encoding::raw,f);
    msu2::encode_frame(Bytes{0,0},{0,0,1,1},msu2::Encoding::raw,f);
    check(f.wire_bytes()==408 && f.pages==1,"output object reusable");
}

class FakeTransport final: public msu2::Transport {
public:
    std::deque<Bytes> input;
    Bytes output, parser;
    std::vector<std::size_t> write_lengths;
    std::vector<msu2::Milliseconds> write_budgets;
    std::vector<std::string> events;
    bool echo_handshake=true, ack=true, ack_exact=false, corrupt_echo=false;
    int discard_count=0, reads=0, fail_write_call=-1;
    bool read_failure=false, fail_payload=false, discard_failure=false, invalid_read_count=false;
    msu2::Milliseconds payload_delay{0};
    void push(const Bytes& bytes, std::size_t fragment=1) {
        for (std::size_t p=0;p<bytes.size();p+=fragment)
            input.emplace_back(bytes.begin()+p,bytes.begin()+std::min(p+fragment,bytes.size()));
    }
    void greeting(const Bytes& bytes=Bytes{0,'M','S','N','1','6'}) {
        push(Bytes{0xAA,'M',0,'M','S',0,'N','x','y'}); push(bytes);
    }
    std::size_t read(std::uint8_t* dst,std::size_t capacity,msu2::Milliseconds timeout) override {
        ++reads;
        if (read_failure) throw std::runtime_error("fake disconnect");
        if (invalid_read_count) return capacity+1;
        if (input.empty()) { std::this_thread::sleep_for(std::min(timeout,msu2::Milliseconds{1})); return 0; }
        auto& first=input.front(); const auto n=std::min(capacity,first.size());
        std::copy_n(first.begin(),n,dst); first.erase(first.begin(),first.begin()+n);
        if (first.empty()) input.pop_front();
        return n;
    }
    void write_all(const std::uint8_t* src,std::size_t length,msu2::Milliseconds timeout) override {
        events.emplace_back("write");
        if (static_cast<int>(write_lengths.size())==fail_write_call || (fail_payload && length && src[0]==4))
            throw std::runtime_error("fake write failure");
        if (length && src[0]==4 && payload_delay.count()) {
            write_budgets.push_back(timeout);
            if (payload_delay>timeout) throw std::runtime_error("fake payload write timeout");
            std::this_thread::sleep_for(payload_delay);
        }
        write_lengths.push_back(length); output.insert(output.end(),src,src+length);
        parser.insert(parser.end(),src,src+length);
        while (parser.size()>=6) {
            Bytes c(parser.begin(),parser.begin()+6); parser.erase(parser.begin(),parser.begin()+6);
            if (c==Bytes(msu2::handshake_command.begin(),msu2::handshake_command.end())) {
                if (echo_handshake) { push(Bytes{0x19,0,'M','S','N','X','N'}); if (corrupt_echo) c[5]='X'; push(c); }
            } else if (c==Bytes{2,3,7,0,0,0} || (c[0]==2 && c[1]==3 && c[2]==10)) {
                if (ack) { push(Bytes{0x13,2,1,0x99}); push(ack_exact?c:Bytes{2,3,0xE1,0xA2,0xB3,0xC4}); }
            }
        }
    }
    void discard_input() override {
        ++discard_count; events.emplace_back("discard");
        if (discard_failure) throw std::runtime_error("fake input purge failure");
        input.clear();
    }
};
msu2::SessionOptions fast_options(msu2::AckMode mode=msu2::AckMode::prefix) {
    msu2::SessionOptions o; o.handshake_timeout=msu2::Milliseconds{20}; o.io_timeout=msu2::Milliseconds{20};
    o.write_chunk=60; o.ack_mode=mode; return o;
}
msu2::EncodedFrame small_frame() {
    msu2::EncodedFrame f; msu2::encode_frame(Bytes{0xF8,0},{0,0,1,1},msu2::Encoding::raw,f); return f;
}
void session_tests() {
    const auto f=small_frame();
    {
        FakeTransport t; t.greeting(); msu2::Session s(t,fast_options());
        check(s.handshake()==16 && s.connected(),"fragmented noisy greeting and exact echo");
        check(t.output==Bytes({0,'M','S','N','C','N'}),"host handshake command exact");
        t.push(Bytes{2,3,0xAA,0xAA,0xAA,0xAA}); // stale ack must be purged before window.
        const auto start=t.output.size(); const auto prior=t.events.size(); const auto discards=t.discard_count;
        const auto prior_calls=t.write_lengths.size();
        const auto stats=s.send(f);
        check(t.discard_count==discards+1 && t.events[prior]=="discard","each frame clears stale input before any write");
        Bytes expected(f.window.begin(),f.window.end()); expected.insert(expected.end(),f.payload.begin(),f.payload.end());
        check(Bytes(t.output.begin()+start,t.output.end())==expected,"send exactly window+payload; no 09 tail or default orientation");
        check(stats.bytes==f.wire_bytes(),"transfer byte statistics");
        const auto calls=t.write_lengths.size();
        check(stats.write_calls==calls-prior_calls,"transfer write statistics exact");
        for (std::size_t i=1;i<calls;++i) check(t.write_lengths[i]<=60 && t.write_lengths[i]%6==0,"chunk alignment and upper bound");
        check(s.connected(),"successful frame stays connected");
        s.send(f); check(t.output.size()==6+2*f.wire_bytes(),"second frame serialized once");
        s.set_orientation(1);
        check(std::equal(t.output.end()-6,t.output.end(),msu2::Command{2,3,10,1,0,0}.begin()),"orientation writes exact command");
        std::atomic<bool> wrong=false;
        std::thread worker([&]{try{s.send(f);}catch(const std::logic_error&){wrong=true;}}); worker.join();
        check(wrong && s.connected(),"session rejects wrong thread without corrupting connection");
    }
    {
        FakeTransport t; msu2::Session s(t,fast_options());
        rejects([&]{s.send(f);},"send before handshake rejected");
        check(t.output.empty(),"unconnected send never writes");
    }
    for (const auto& malformed:{Bytes{0,'M','S','N',16,0},Bytes{0,'M','S','N','1','x'},Bytes{'M','S','N','1','6'}}) {
        FakeTransport t; t.greeting(malformed); msu2::Session s(t,fast_options());
        const auto start=Clock::now(); rejects([&]{s.handshake();},"malformed greeting fails");
        check(!s.connected() && t.output.empty(),"malformed greeting cannot trigger host wake command");
        check(Clock::now()-start<std::chrono::milliseconds(300),"handshake timeout bounded");
    }
    {
        FakeTransport t; t.greeting(); t.corrupt_echo=true; msu2::Session s(t,fast_options());
        rejects([&]{s.handshake();},"handshake prefix does not substitute for exact MSNCN echo");
        check(!s.connected(),"bad echo stays disconnected");
    }
    {
        FakeTransport t; t.greeting(); t.ack_exact=true; msu2::Session s(t,fast_options(msu2::AckMode::echo));
        s.handshake(); s.send(f); check(s.connected(),"experimental echo mode exact six-byte ack succeeds");
    }
    {
        FakeTransport t; t.greeting(); msu2::Session s(t,fast_options(msu2::AckMode::echo)); s.handshake();
        rejects([&]{s.send(f);},"experimental echo mode rejects only matching prefix");
        check(!s.connected(),"echo ack failure invalidates connection");
    }
    for (int failure=0;failure<5;++failure) {
        FakeTransport t; t.greeting(); msu2::Session s(t,fast_options()); s.handshake();
        if (failure==0) t.ack=false;
        if (failure==1) t.fail_write_call=static_cast<int>(t.write_lengths.size());
        if (failure==2) t.fail_payload=true;
        if (failure==3) t.read_failure=true;
        if (failure==4) t.discard_failure=true;
        const auto start=Clock::now(); rejects([&]{s.send(f);},"send IO/ack failure rejected");
        check(!s.connected(),"all send transport and ack failures invalidate connection");
        check(Clock::now()-start<std::chrono::milliseconds(300),"send failure bounded");
        const auto size=t.output.size(); rejects([&]{s.send(f);},"failed session cannot silently retry");
        check(t.output.size()==size,"invalidated session writes no further data");
    }
    {
        FakeTransport t; t.greeting(); msu2::Session s(t,fast_options()); s.handshake(); t.ack=false;
        t.push(Bytes{2,3}); rejects([&]{s.send(f);},"stale matching ack is discarded and cannot complete new window");
    }
    {
        FakeTransport t; t.greeting(); msu2::Session s(t,fast_options()); s.handshake(); t.payload_delay=msu2::Milliseconds{6};
        const auto start=Clock::now(); rejects([&]{s.send(f);},"payload uses one bounded deadline, not a fresh deadline per chunk");
        check(!s.connected() && Clock::now()-start<std::chrono::milliseconds(100),"global payload deadline invalidates promptly");
        check(t.write_budgets.size()>=2 && t.write_budgets.back()<t.write_budgets.front(),"payload timeout budgets decrease");
    }
    {
        FakeTransport t; t.invalid_read_count=true; msu2::Session s(t,fast_options());
        rejects([&]{s.handshake();},"transport capacity violation rejected during handshake");
        check(!s.connected(),"invalid read count cannot establish a session");
    }
    for (std::size_t chunk:{std::size_t(0),std::size_t(5),std::size_t(7),std::size_t(65538)}) {
        FakeTransport t; auto o=fast_options(); o.write_chunk=chunk;
        rejects([&]{msu2::Session s(t,o);},"invalid Session write chunk rejected");
    }
    for (int timeout:{0,-1}) {
        FakeTransport t; auto o=fast_options(); o.io_timeout=msu2::Milliseconds(timeout);
        rejects([&]{msu2::Session s(t,o);},"nonpositive Session timeout rejected");
    }
}

struct TempFiles {
    std::filesystem::path dir;
    TempFiles() {
        dir=std::filesystem::temp_directory_path()/ ("msu2-core-"+std::to_string(Clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(dir);
    }
    ~TempFiles() { std::error_code ec; std::filesystem::remove_all(dir,ec); }
    std::string put(const std::string& name,const std::string& data) {
        auto p=dir/name; std::ofstream stream(p,std::ios::binary); stream.write(data.data(),static_cast<std::streamsize>(data.size()));
        if (!stream) throw std::runtime_error("test fixture write failed");
        return p.string();
    }
};
void image_tests() {
    TempFiles files;
    const auto p3=files.put("valid-p3.ppm","P3\n# dimensions\n2 1\n15\n15 0 0 # between samples\n0 15 15\n");
    auto img=msu2::read_ppm(p3);
    check(img.width==2 && img.height==1 && img.rgb==Bytes({255,0,0,0,255,255}),"P3 comments and maxval scale");
    std::string p6="P6\n2 1\n255\n"; p6.append("\x20\x23\x0A\x00\x0D\xFF",6);
    img=msu2::read_ppm(files.put("valid-p6.ppm",p6));
    check(img.rgb==Bytes({32,35,10,0,13,255}),"P6 binary-leading whitespace/hash and NUL preserved");
    std::string p6scale="P6\n1 1\n15\n"; p6scale.append("\x0F\x00\x0F",3);
    check(msu2::read_ppm(files.put("p6-scale.ppm",p6scale)).rgb==Bytes({255,0,255}),"P6 maxval scale");
    const auto out=(files.dir/"roundtrip.ppm").string(); msu2::write_ppm(out,img);
    check(msu2::read_ppm(out).rgb==img.rgb,"PPM binary round trip");
    const std::vector<std::string> bad={"","P5\n1 1\n255\nxxx","P3\n0 1\n255\n0 0 0",
        "P3\n-1 1\n255\n0 0 0","P3\n1 1\n0\n0 0 0","P3\n1 1\n256\n0 0 0",
        "P3\n1 1\n255\n0 0","P3\n1 1\n255\n0 0 256","P3\n1 1\n255\n0 0 -1",
        "P3\n1 1\n255\n0 0 garbage","P6\n1 1\n255\nxx","P6\n1 1\n255",
        "P3\n2147483647 2147483647\n255\n", "P3\n16777217 1\n255\n","P3\n999999999999999999999 1\n255\n"};
    for (std::size_t i=0;i<bad.size();++i) {
        const auto p=files.put("bad-"+std::to_string(i)+".ppm",bad[i]);
        rejects([&]{msu2::read_ppm(p);},"malformed PPM rejected "+std::to_string(i));
    }
    rejects([&]{msu2::read_ppm((files.dir/"missing.ppm").string());},"missing PPM rejected");
    msu2::Image colors{5,1,Bytes{255,0,0,0,255,0,0,0,255,255,255,255,0,0,0}};
    Bytes packed; msu2::to_rgb565(colors,packed);
    check(packed==Bytes({0xF8,0,0x07,0xE0,0,0x1F,0xFF,0xFF,0,0}),"RGB565 golden primaries, white, black, big-endian");
    msu2::Image solid{3,3,Bytes(27,117)};
    const auto sized=msu2::resize_crop(solid,160,80);
    check(sized.width==160 && sized.height==80 && sized.rgb==Bytes(38400,117),"resize/crop solid exact");
    // A 6x2 image has a centered four-column crop. Bilinear interpolation may
    // blend the boundary samples, so verify the interior and symmetric edges.
    msu2::Image strip{6,2,Bytes(36)};
    for (int y=0;y<2;++y) for (int x=0;x<6;++x) {
        const auto p=std::size_t(y*6+x)*3; strip.rgb[p]=x==0?255:0; strip.rgb[p+1]=(x>0&&x<5)?255:0; strip.rgb[p+2]=x==5?255:0;
    }
    const auto crop=msu2::resize_crop(strip,160,80);
    bool green=true;
    for (int y=0;y<80;++y) for (int x=20;x<140;++x) {
        const auto i=std::size_t(y*160+x)*3;
        green=green && crop.rgb[i]==0 && crop.rgb[i+1]==255 && crop.rgb[i+2]==0;
    }
    check(green,"centered crop retains green interior");
    check(crop.rgb[0]==crop.rgb[(159*3)+2] && crop.rgb[1]==crop.rgb[(159*3)+1] &&
          crop.rgb[0]<128 && crop.rgb[2]==0 && crop.rgb[159*3]==0,"centered crop symmetric boundary blend");
    for (const auto& malformed:{msu2::Image{},msu2::Image{1,1,{}},msu2::Image{1,1,Bytes(4)},msu2::Image{-1,1,Bytes(3)}}) {
        rejects([&]{msu2::to_rgb565(malformed,packed);},"malformed RGB888 image rejected");
        rejects([&]{msu2::resize_crop(malformed,160,80);},"malformed resize source rejected");
        rejects([&]{msu2::write_ppm(out,malformed);},"malformed PPM output rejected");
    }
    rejects([&]{msu2::resize_crop(solid,0,80);},"zero resize width rejected");
    for (const auto& name:{"bars","solid","checker","gradient","noise","moving"}) {
        const auto pattern=msu2::make_pattern(name,17);
        check(pattern.width==160 && pattern.height==80 && pattern.rgb.size()==38400,"pattern dimensions "+std::string(name));
        msu2::to_rgb565(pattern,packed); check(packed.size()==25600,"pattern conversion "+std::string(name));
    }
    rejects([]{msu2::make_pattern("unknown");},"unknown pattern rejected");
}
void latest_tests() {
    msu2::LatestFrame<int> q;
    q.put(1); int current=0; check(q.take(current) && current==1,"take in-flight frame");
    q.put(2); q.put(3); q.put(4);
    check(current==1,"in-flight frame never overwritten");
    check(q.dropped()==2 && q.take(current) && current==4,"backpressure drops old waiting frames and retains latest");
    q.put(5); q.close(); check(q.take(current) && current==5,"close drains one pending frame");
    check(!q.take(current),"closed empty queue stops consumer");
    q.put(6); check(!q.take(current) && q.dropped()==2,"put after close ignored");
    msu2::LatestFrame<int> blocking;
    std::atomic<bool> done=false;
    std::thread consumer([&]{int value=0; check(!blocking.take(value),"close wakes blocked consumer"); done=true;});
    blocking.close(); consumer.join(); check(done,"consumer woke");
    msu2::LatestFrame<int> failed;
    failed.close(std::make_exception_ptr(std::runtime_error("capture error")));
    rejects([&]{failed.take(current);},"producer exception reaches consumer");
}
}
int main() {
    try {
        protocol_tests(); std::cout << "PASS protocol golden/independent decode\n";
        image_tests(); std::cout << "PASS PPM/RGB565/crop/patterns\n";
        session_tests(); std::cout << "PASS fake transport handshake/ACK/failure/thread/chunk\n";
        latest_tests(); std::cout << "PASS latest-frame backpressure\n";
        std::cout << "PASS " << checks << " assertions\n"; return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL after " << checks << " assertions: " << e.what() << '\n'; return 1; }
}
