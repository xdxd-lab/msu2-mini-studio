#include "msu2app/preview.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace msu2app {
namespace {
double linear(double v){return v<=0.04045?v/12.92:std::pow((v+0.055)/1.055,2.4);}
double luminance(unsigned r,unsigned g,unsigned b){return .2126*linear(r/255.)+.7152*linear(g/255.)+.0722*linear(b/255.);}
double contrast(double a,double b){return (std::max(a,b)+.05)/(std::min(a,b)+.05);}
}
EdgeContrast analyze_preview_edges(const std::vector<std::uint8_t>& data){
    if(data.size()!=160*80*2)throw std::invalid_argument("preview needs complete 160x80 RGB565");
    const double dark=luminance(17,21,28),light=luminance(242,245,250);
    EdgeContrast result{21,21,PreviewBorder::dual};
    for(int y=0;y<80;++y)for(int x=0;x<160;++x){
        if(x>=2&&x<158&&y>=2&&y<78)continue;
        const auto i=(y*160+x)*2;const unsigned p=(unsigned(data[i])<<8)|data[i+1];
        const double l=luminance(((p>>11)&31)*255/31,((p>>5)&63)*255/63,(p&31)*255/31);
        result.dark_min=std::min(result.dark_min,contrast(l,dark));result.light_min=std::min(result.light_min,contrast(l,light));
    }
    if(result.light_min>=3 && result.light_min>=result.dark_min)result.choice=PreviewBorder::light;
    else if(result.dark_min>=3)result.choice=PreviewBorder::dark;
    else if(result.light_min>=3)result.choice=PreviewBorder::light;
    return result;
}
PreviewBorder BorderStability::update(PreviewBorder desired,double now){
    if(!std::isfinite(now)||now<0)throw std::invalid_argument("invalid preview clock");
    if(desired==PreviewBorder::dual){current_=candidate_=desired;since_=now;return current_;}
    if(desired==current_)return current_;
    if(candidate_!=desired||since_<0||now<since_){candidate_=desired;since_=now;current_=PreviewBorder::dual;}
    if(now-since_>=250)current_=desired;
    return current_;
}
void BorderStability::reset(){current_=candidate_=PreviewBorder::dual;since_=-1;}
}
