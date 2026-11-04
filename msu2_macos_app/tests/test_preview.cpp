#include "msu2app/preview.hpp"
#include "msu2/image.hpp"
#include <cassert>
#include <iostream>
using namespace msu2app;
std::vector<std::uint8_t> solid(unsigned value){std::vector<std::uint8_t> bytes(25600);for(size_t i=0;i<bytes.size();i+=2){bytes[i]=value>>8;bytes[i+1]=value;}return bytes;}
int main(){
    auto black=solid(0),white=solid(0xffff);auto before=black;
    assert(analyze_preview_edges(black).choice==PreviewBorder::light);assert(analyze_preview_edges(black).light_min>=3);assert(before==black);
    assert(analyze_preview_edges(white).choice==PreviewBorder::dark);assert(analyze_preview_edges(white).dark_min>=3);
    for(bool horizontal:{false,true}){auto mixed=black;for(int y=0;y<80;++y)for(int x=0;x<160;++x)if(horizontal?y>=40:x>=80){auto p=(y*160+x)*2;mixed[p]=mixed[p+1]=255;}assert(analyze_preview_edges(mixed).choice==PreviewBorder::dual);}
    // A bright center must not defeat a dark boundary; a single light edge sample must not be averaged out.
    auto center=white;for(int y=0;y<80;++y)for(int x=0;x<160;++x)if(x<2||x>=158||y<2||y>=78){auto p=(y*160+x)*2;center[p]=center[p+1]=0;}
    assert(analyze_preview_edges(center).choice==PreviewBorder::light);center[159*2]=center[159*2+1]=255;assert(analyze_preview_edges(center).choice==PreviewBorder::dual);
    for(const auto& name:{"bars","checker","noise"}){auto image=msu2::make_pattern(name);std::vector<std::uint8_t> rgb;msu2::to_rgb565(image,rgb);auto original=rgb;(void)analyze_preview_edges(rgb);assert(rgb==original);assert(analyze_preview_edges(rgb).choice==PreviewBorder::dual);}
    bool rejected=false;try{analyze_preview_edges({0,0});}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
    BorderStability stable;assert(stable.update(PreviewBorder::light,0)==PreviewBorder::dual);assert(stable.update(PreviewBorder::light,249)==PreviewBorder::dual);assert(stable.update(PreviewBorder::light,250)==PreviewBorder::light);
    assert(stable.update(PreviewBorder::dark,251)==PreviewBorder::dual);assert(stable.update(PreviewBorder::light,400)==PreviewBorder::dual);assert(stable.update(PreviewBorder::light,650)==PreviewBorder::light);
    assert(stable.update(PreviewBorder::dual,651)==PreviewBorder::dual);stable.reset();assert(stable.update(PreviewBorder::dark,900)==PreviewBorder::dual);assert(stable.update(PreviewBorder::dark,1150)==PreviewBorder::dark);
    std::cout<<"PASS RGB565 edge contrast, mixed borders, hysteresis and immutable source bytes\n";
}
