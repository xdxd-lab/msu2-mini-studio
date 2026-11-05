#include "msu2/protocol.hpp"
#include <algorithm>
#include <stdexcept>

namespace msu2 {
void validate_rect(Rect r) {
    if(r.x<0 || r.y<0 || r.width<=0 || r.height<=0 ||
       r.x>=screen_width || r.y>=screen_height ||
       r.width>screen_width-r.x || r.height>screen_height-r.y)
        throw std::invalid_argument("rectangle must fit MINI 160x80");
}
Command orientation_command(unsigned value) {
    if(value>1) throw std::invalid_argument("orientation must be firmware state 0 or 1");
    return {2,3,10,static_cast<std::uint8_t>(value),0,0};
}
namespace {
void append(std::vector<std::uint8_t>& out, Command c) { out.insert(out.end(),c.begin(),c.end()); }
std::uint32_t word(const std::uint8_t* p) {
    return (std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)|
           (std::uint32_t(p[2])<<8)|p[3];
}
struct CountTable {
    std::array<std::uint32_t,128> keys{};
    std::array<unsigned char,128> counts{};
    std::size_t slot(std::uint32_t key) const {
        std::size_t s=(key*std::uint32_t(2654435761u))>>25;
        while(counts[s] && keys[s]!=key) s=(s+1)&127;
        return s;
    }
};
}
void encode_frame(const std::vector<std::uint8_t>& pixels, Rect r,
                  Encoding encoding, EncodedFrame& out) {
    validate_rect(r);
    const auto total=std::size_t(r.width)*std::size_t(r.height)*2;
    if(pixels.size()!=total) throw std::invalid_argument("RGB565 byte count must equal width*height*2");
    if(encoding!=Encoding::raw && encoding!=Encoding::pair)
        throw std::invalid_argument("unknown encoding");
    out.window={2,0,std::uint8_t(r.x>>8),std::uint8_t(r.x),std::uint8_t(r.y>>8),std::uint8_t(r.y),
                2,1,std::uint8_t(r.width>>8),std::uint8_t(r.width),std::uint8_t(r.height>>8),std::uint8_t(r.height),
                2,3,7,0,0,0};
    out.raw_bytes=total; out.pages=(total+255)/256;
    out.payload.clear(); out.payload.reserve(out.pages*390);
    for(std::size_t pos=0;pos<total;pos+=256) {
        const auto valid=std::min(std::size_t(256),total-pos);
        std::array<std::uint8_t,256> page;
        page.fill(0xff);
        std::copy_n(pixels.data()+pos,valid,page.begin());
        const bool compressed=encoding==Encoding::pair && valid==256;
        std::uint32_t mode=0;
        if(compressed) {
            CountTable table;
            for(std::size_t i=0;i<64;++i) {
                auto key=word(page.data()+i*4); auto s=table.slot(key);
                table.keys[s]=key; ++table.counts[s];
            }
            unsigned best=0;
            // A stable tie rule: first occurrence among final-frequency ties.
            for(std::size_t i=0;i<64;++i) {
                auto key=word(page.data()+i*4); auto c=table.counts[table.slot(key)];
                if(c>best) {best=c;mode=key;}
            }
            append(out.payload,{2,4,std::uint8_t(mode>>24),std::uint8_t(mode>>16),
                                std::uint8_t(mode>>8),std::uint8_t(mode)});
        }
        for(std::size_t i=0;i<64;++i) {
            const auto* p=page.data()+i*4;
            if(!compressed || word(p)!=mode)
                append(out.payload,{4,std::uint8_t(i),p[0],p[1],p[2],p[3]});
        }
        append(out.payload,{2,3,8,std::uint8_t(valid>>8),std::uint8_t(valid),0});
    }
}
}
