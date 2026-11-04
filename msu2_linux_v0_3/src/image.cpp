#include "msu2/image.hpp"
#include "msu2/protocol.hpp"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace msu2 {
namespace {
std::size_t pixel_count(int w,int h) {
    if(w<=0 || h<=0 || w>16384 || h>16384 || std::uint64_t(w)*std::uint64_t(h)>16000000)
        throw std::invalid_argument("image dimensions exceed 16 million pixels or are invalid");
    return std::size_t(w)*std::size_t(h);
}
void validate(const Image& im) {
    if(im.rgb.size()!=pixel_count(im.width,im.height)*3)
        throw std::invalid_argument("RGB888 image size mismatch");
}
std::string token(std::istream& in) {
    for(;;) {
        int c=in.peek();
        if(c==EOF) throw std::runtime_error("truncated PPM header/data");
        if(std::isspace(static_cast<unsigned char>(c))) {in.get();continue;}
        if(c=='#') {in.ignore(std::numeric_limits<std::streamsize>::max(),'\n');continue;}
        break;
    }
    std::string s;
    while(in.good()) {
        int c=in.peek();
        if(c==EOF || c=='#' || std::isspace(static_cast<unsigned char>(c))) break;
        s.push_back(static_cast<char>(in.get()));
        if(s.size()>32) throw std::runtime_error("PPM token too long");
    }
    return s;
}
int number(const std::string& s) {
    if(s.empty() || !std::all_of(s.begin(),s.end(),[](unsigned char c){return c>='0' && c<='9';}))
        throw std::runtime_error("invalid PPM integer");
    unsigned long long value=0;
    for(auto c:s) {value=value*10+unsigned(c-'0'); if(value>16384000) throw std::runtime_error("PPM integer too large");}
    return static_cast<int>(value);
}
}
Image read_ppm(const std::string& path) {
    std::ifstream in(path,std::ios::binary);
    if(!in) throw std::runtime_error("cannot open PPM: "+path);
    auto magic=token(in);
    if(magic!="P6" && magic!="P3") throw std::runtime_error("input must be P6 or P3 PPM");
    Image im; im.width=number(token(in)); im.height=number(token(in));
    const auto count=pixel_count(im.width,im.height)*3;
    auto maxval=number(token(in));
    if(maxval<1 || maxval>255) throw std::runtime_error("PPM maxval must be 1..255 (8-bit samples)");
    im.rgb.resize(count);
    if(magic=="P6") {
        int delimiter=in.get();
        if(delimiter==EOF || !std::isspace(static_cast<unsigned char>(delimiter)))
            throw std::runtime_error("PPM binary header missing whitespace separator");
        if(delimiter=='\r' && in.peek()=='\n') in.get();
        in.read(reinterpret_cast<char*>(im.rgb.data()),static_cast<std::streamsize>(count));
        if(in.gcount()!=static_cast<std::streamsize>(count)) throw std::runtime_error("truncated PPM pixels");
        for(auto& v:im.rgb) {
            if(v>maxval) throw std::runtime_error("PPM sample exceeds maxval");
            v=static_cast<std::uint8_t>(unsigned(v)*255/unsigned(maxval));
        }
    } else {
        for(auto& v:im.rgb) {
            auto sample=number(token(in));
            if(sample>maxval) throw std::runtime_error("PPM sample exceeds maxval");
            v=static_cast<std::uint8_t>(unsigned(sample)*255/unsigned(maxval));
        }
    }
    return im;
}
void write_ppm(const std::string& path,const Image& im) {
    validate(im);
    std::ofstream out(path,std::ios::binary);
    if(!out) throw std::runtime_error("cannot create PPM: "+path);
    out<<"P6\n"<<im.width<<' '<<im.height<<"\n255\n";
    out.write(reinterpret_cast<const char*>(im.rgb.data()),static_cast<std::streamsize>(im.rgb.size()));
    if(!out) throw std::runtime_error("failed writing PPM");
}
Image resize_crop(const Image& src,int w,int h) {
    validate(src); auto n=pixel_count(w,h);
    if(w==src.width && h==src.height) return src;
    Image dst{w,h,std::vector<std::uint8_t>(n*3)};
    const double scale=std::min(double(src.width)/w,double(src.height)/h);
    const double left=(src.width-w*scale)/2, top=(src.height-h*scale)/2;
    for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
        const double sx=std::clamp(left+(x+0.5)*scale-0.5,0.0,double(src.width-1));
        const double sy=std::clamp(top+(y+0.5)*scale-0.5,0.0,double(src.height-1));
        const int x0=int(sx), y0=int(sy), x1=std::min(x0+1,src.width-1), y1=std::min(y0+1,src.height-1);
        const double ax=sx-x0, ay=sy-y0;
        for(int c=0;c<3;++c) {
            auto at=[&](int xx,int yy){return src.rgb[(std::size_t(yy)*src.width+xx)*3+c];};
            const double v=(1-ay)*((1-ax)*at(x0,y0)+ax*at(x1,y0))+ay*((1-ax)*at(x0,y1)+ax*at(x1,y1));
            dst.rgb[(std::size_t(y)*w+x)*3+c]=std::uint8_t(v+0.5);
        }
    }
    return dst;
}
void to_rgb565(const Image& im,std::vector<std::uint8_t>& out) {
    validate(im); const auto n=std::size_t(im.width)*im.height;
    out.resize(n*2);
    for(std::size_t i=0;i<n;++i) {
        const auto* p=im.rgb.data()+3*i;
        const auto v=std::uint16_t((std::uint16_t(p[0]>>3)<<11)|(std::uint16_t(p[1]>>2)<<5)|(p[2]>>3));
        out[2*i]=std::uint8_t(v>>8);out[2*i+1]=std::uint8_t(v);
    }
}
Image make_pattern(const std::string& name,unsigned tick) {
    if(name!="bars" && name!="solid" && name!="checker" && name!="gradient" && name!="noise" && name!="moving")
        throw std::invalid_argument("unknown pattern; use bars, solid, checker, gradient, noise, moving");
    Image im{160,80,std::vector<std::uint8_t>(160*80*3)};
    constexpr unsigned char colors[8][3]={{255,255,255},{255,255,0},{0,255,255},{0,255,0},{255,0,255},{255,0,0},{0,0,255},{0,0,0}};
    std::uint32_t rng=0x6d737532u^(tick*0x9e3779b9u);
    if(!rng) rng=1;
    for(unsigned y=0;y<80;++y) for(unsigned x=0;x<160;++x) {
        auto* p=im.rgb.data()+(y*160+x)*3;
        if(name=="noise") {
            for(int c=0;c<3;++c) {rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;p[c]=std::uint8_t(rng);}
        } else if(name=="gradient") {
            p[0]=std::uint8_t(x*255/159);p[1]=std::uint8_t(y*255/79);p[2]=std::uint8_t((x+y+tick)%256);
        } else if(name=="solid") {p[0]=255;p[1]=0;p[2]=0;}
        else if(name=="checker") {p[0]=p[1]=p[2]=((x/8+y/8)%2)?255:0;}
        else {
            auto col=(x/20)%8;
            for(int c=0;c<3;++c)p[c]=colors[col][c];
            if(name=="moving" && ((x+160-(tick%160))%160)<12 && y>=28 && y<52)
                for(int c=0;c<3;++c)p[c]=255-p[c];
        }
    }
    return im;
}
}
