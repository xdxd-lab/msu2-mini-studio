// Delta updates are decoded as firmware commands onto a retained LCD canvas.
#include "msu2/delta.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Bytes=std::vector<std::uint8_t>;
constexpr std::size_t pixels=160*80;
int checks=0;
void check(bool value,const std::string& message) {
    ++checks;
    if(!value)throw std::runtime_error(message);
}
template<class F>void rejects(F&& f,const std::string& message) {
    bool rejected=false;
    try{f();}catch(const std::invalid_argument&){rejected=true;}
    check(rejected,message);
}
unsigned be16(const std::uint8_t* p){return unsigned(p[0])*256+p[1];}
Bytes random_image(unsigned seed) {
    std::mt19937 rng(seed);Bytes image(pixels*2);
    for(auto& b:image)b=static_cast<std::uint8_t>(rng());
    return image;
}
void set_pixel(Bytes& image,int x,int y,unsigned value) {
    const auto i=std::size_t(y*160+x)*2;
    image[i]=static_cast<std::uint8_t>(value>>8);image[i+1]=static_cast<std::uint8_t>(value);
}
void invert_pixel(Bytes& image,int x,int y) {
    const auto i=std::size_t(y*160+x)*2;
    image[i]^=0xff;image[i+1]^=0xff;
}
std::size_t changes(const Bytes& previous,const Bytes& current) {
    std::size_t count=0;
    for(std::size_t p=0;p<pixels;++p)
        count+=previous[p*2]!=current[p*2]||previous[p*2+1]!=current[p*2+1];
    return count;
}
// Decode independently from the documented six-byte firmware command grammar.
Bytes decode(const msu2::Patch& patch) {
    const auto& r=patch.rect;const auto& frame=patch.frame;const auto& w=frame.window;
    check(r.x>=0&&r.y>=0&&r.width>0&&r.height>0&&r.x+r.width<=160&&r.y+r.height<=80,"valid patch bounds");
    check(w[0]==2&&w[1]==0&&be16(w.data()+2)==unsigned(r.x)&&be16(w.data()+4)==unsigned(r.y),"patch XY bytes");
    check(w[6]==2&&w[7]==1&&be16(w.data()+8)==unsigned(r.width)&&be16(w.data()+10)==unsigned(r.height),"patch size bytes");
    check(std::equal(w.begin()+12,w.end(),msu2::Command{2,3,7,0,0,0}.begin()),"patch start command");
    check(frame.payload.size()%6==0,"patch command alignment");
    std::array<std::uint8_t,256> ram{};std::array<bool,64> initialized{};
    Bytes out;std::size_t page_commands=0,page_count=0;
    for(std::size_t i=0;i<frame.payload.size();i+=6) {
        const auto* c=frame.payload.data()+i;++page_commands;
        if(c[0]==4) {
            check(c[1]<64,"RAM register bounds");
            std::copy_n(c+2,4,ram.begin()+c[1]*4);initialized[c[1]]=true;
        }else if(c[0]==2&&c[1]==4) {
            for(std::size_t k=0;k<64;++k)std::copy_n(c+2,4,ram.begin()+k*4);
            initialized.fill(true);
        }else if(c[0]==2&&c[1]==3&&c[2]==8) {
            const auto n=be16(c+3);
            check(n>0&&n<=256&&n%2==0&&c[5]==0,"commit size/reserved byte");
            check(std::all_of(initialized.begin(),initialized.end(),[](bool v){return v;}),"all page registers initialized");
            if(n<256) {
                check(page_commands==65,"partial page has raw stages plus commit");
                check(std::all_of(ram.begin()+n,ram.end(),[](std::uint8_t v){return v==255;}),"partial page FF padding");
            }
            out.insert(out.end(),ram.begin(),ram.begin()+n);
            initialized.fill(false);page_commands=0;++page_count;
        }else throw std::runtime_error("unexpected patch command");
    }
    check(page_commands==0,"complete patch page stream");
    check(out.size()==std::size_t(r.width*r.height)*2,"patch decoded dimensions");
    check(frame.raw_bytes==out.size()&&frame.pages==page_count,"patch frame metadata");
    return out;
}
void verify(const msu2::UpdatePlan& plan,const Bytes& before,const Bytes& current,bool first=false) {
    check(plan.changed_pixels==(first?pixels:changes(before,current)),"RGB565 pixel change count");
    check(plan.patches.size()<=32,"at most 32 windows");
    Bytes canvas=before;std::size_t transmitted=0,wire=0;
    for(const auto& patch:plan.patches) {
        const auto decoded=decode(patch);const auto& r=patch.rect;
        const auto previous=canvas;
        for(int y=0;y<80;++y)for(int x=0;x<160;++x) {
            const auto dst=std::size_t(y*160+x)*2;
            if(x>=r.x&&x<r.x+r.width&&y>=r.y&&y<r.y+r.height) {
                const auto src=std::size_t((y-r.y)*r.width+x-r.x)*2;
                check(decoded[src]==current[dst]&&decoded[src+1]==current[dst+1],"packed patch rows match full source");
                canvas[dst]=decoded[src];canvas[dst+1]=decoded[src+1];
            }else check(canvas[dst]==previous[dst]&&canvas[dst+1]==previous[dst+1],"window preserves outside LCD pixels");
        }
        transmitted+=decoded.size()/2;wire+=patch.frame.wire_bytes();
    }
    check(canvas==current,"all decoded patches reconstruct the full current LCD canvas");
    check(plan.transmitted_pixels==transmitted&&plan.wire_bytes==wire,"actual patch wire/transmitted totals");
    if(plan.kind==msu2::UpdateKind::unchanged)
        check(plan.patches.empty()&&wire==0&&transmitted==0&&plan.changed_pixels==0,"unchanged sends nothing");
    else check(!plan.patches.empty(),"changed/forced update has windows");
}
std::size_t score(const msu2::UpdatePlan& p){return p.wire_bytes+384*p.patches.size();}
std::size_t rectangle_score(const Bytes& image,msu2::Rect r,msu2::Encoding mode) {
    Bytes crop;
    for(int y=r.y;y<r.y+r.height;++y) {
        const auto start=image.begin()+std::size_t(y*160+r.x)*2;
        crop.insert(crop.end(),start,start+r.width*2);
    }
    msu2::EncodedFrame encoded;msu2::encode_frame(crop,r,mode,encoded);
    return encoded.wire_bytes()+384;
}
void lifecycle_tests(msu2::Encoding mode) {
    msu2::DeltaPlanner planner;const Bytes base=random_image(17);
    check(!planner.has_reference(),"new planner has no retained LCD reference");
    auto first=planner.plan(base,mode);
    check(first.kind==msu2::UpdateKind::full&&first.patches.size()==1,"first update is full");
    verify(first,Bytes(pixels*2),base,true);
    check(!planner.has_reference(),"planning first frame does not commit it");
    check(planner.plan(base,mode).kind==msu2::UpdateKind::full,"uncommitted first plan stays full");
    planner.commit(base);check(planner.has_reference(),"successful commit establishes reference");
    verify(planner.plan(base,mode),base,base);
    auto key=planner.plan(base,mode,true);
    check(key.kind==msu2::UpdateKind::full&&key.changed_pixels==0,"unchanged forced keyframe is full with zero changed pixels");
    verify(key,base,base);
    auto dropped=base;invert_pixel(dropped,3,4);
    auto dropped_plan=planner.plan(dropped,mode);verify(dropped_plan,base,dropped);
    auto next=base;invert_pixel(next,151,72);
    auto next_plan=planner.plan(next,mode);verify(next_plan,base,next);
    check(next_plan.changed_pixels==1,"dropped planned capture is not a reference");
    planner.commit(next);
    auto later=next;invert_pixel(later,77,33);
    auto failed=planner.plan(later,mode);verify(failed,next,later);
    // A sender that aborts before committing must retain the last complete source.
    verify(planner.plan(next,mode),next,next);
    planner.invalidate();check(!planner.has_reference(),"failure/reconnect invalidates reference");
    auto recovery=planner.plan(later,mode);
    check(recovery.kind==msu2::UpdateKind::full,"reference invalidation forces recovery full update");
    verify(recovery,next,later,true);
    planner.commit(base);
    for(std::size_t length:{std::size_t(0),std::size_t(1),std::size_t(25599),std::size_t(25601)}) {
        rejects([&]{planner.plan(Bytes(length),mode);},"bad planning source size rejected");
        rejects([&]{planner.commit(Bytes(length));},"bad committed source size rejected");
        verify(planner.plan(base,mode),base,base);
    }
    rejects([&]{planner.plan(base,static_cast<msu2::Encoding>(99));},"invalid encoding rejected even unchanged");
}
void geometry_tests() {
    const Bytes base=random_image(31);
    for(auto mode:{msu2::Encoding::raw,msu2::Encoding::pair}) {
        msu2::DeltaPlanner planner;planner.commit(base);
        for(auto xy:{std::array<int,2>{0,0},{159,0},{0,79},{159,79}}) {
            auto current=base;invert_pixel(current,xy[0],xy[1]);
            auto p=planner.plan(current,mode);verify(p,base,current);
            check(p.changed_pixels==1&&p.wire_bytes==408&&p.patches.size()==1,"single corner has exact raw tail wire size");
        }
        auto one_byte=base;one_byte[2*(17*160+23)+1]^=1;
        verify(planner.plan(one_byte,mode),base,one_byte);
        check(planner.plan(one_byte,mode).changed_pixels==1,"one changed RGB565 byte counts one pixel");
        auto odd=base;
        for(int y=9;y<52;++y)for(int x=37;x<40;++x)invert_pixel(odd,x,y);
        auto odd_plan=planner.plan(odd,mode);verify(odd_plan,base,odd);
        check(odd_plan.changed_pixels==129&&odd_plan.patches.size()==1,"odd-width multirow/page-boundary patch");
        check(score(odd_plan)<=rectangle_score(odd,{37,9,3,43},mode),"chooses no more than actual encoded bounding cost");
        auto wrap=base;
        for(int y=28;y<52;++y)for(int x:{0,1,158,159})invert_pixel(wrap,x,y);
        auto p=planner.plan(wrap,mode);verify(p,base,wrap);
        check(p.patches.size()==2&&p.kind==msu2::UpdateKind::runs,"wrapped edge strips become two vertically merged exact runs");
        auto corners=base;
        for(auto xy:{std::array<int,2>{0,0},{159,0},{0,79},{159,79}})invert_pixel(corners,xy[0],xy[1]);
        p=planner.plan(corners,mode);verify(p,base,corners);
        check(p.patches.size()==4&&p.changed_pixels==4,"four distant corners do not expand into an expensive full window");
        auto tiled=base;
        for(int y=0;y<8;y+=2)for(int x=0;x<8;x+=2) {
            invert_pixel(tiled,x,y);invert_pixel(tiled,152+x,72+y);
        }
        p=planner.plan(tiled,mode);verify(p,base,tiled);
        check(p.kind==msu2::UpdateKind::tiles&&p.patches.size()==2,"tile strategy beats many isolated run windows");
        check(score(p)<=rectangle_score(tiled,{0,0,8,8},mode)+rectangle_score(tiled,{152,72,8,8},mode),"tile score uses actual encoded pages plus window penalty");
        auto sparse=base;
        for(int y=0;y<80;y+=8)for(int x=0;x<160;x+=8)invert_pixel(sparse,x,y);
        p=planner.plan(sparse,mode);verify(p,base,sparse);
        check(p.changed_pixels==200&&p.patches.size()<=32,"over-cap isolated exact runs use a bounded candidate");
        auto noise=random_image(32);p=planner.plan(noise,mode);verify(p,base,noise);
        check(p.kind==msu2::UpdateKind::full&&p.patches.size()==1,"dense noise falls back to one full update");
        check(p.wire_bytes==p.patches[0].frame.wire_bytes(),"window penalty is excluded from wire bytes");
    }
    // Compression can make one full window cheaper than four one-pixel tails.
    Bytes uniform(pixels*2,0);auto corners=uniform;
    for(auto xy:{std::array<int,2>{0,0},{159,0},{0,79},{159,79}})set_pixel(corners,xy[0],xy[1],0xffff);
    msu2::DeltaPlanner planner;planner.commit(uniform);
    auto p=planner.plan(corners,msu2::Encoding::pair);verify(p,uniform,corners);
    check(p.kind==msu2::UpdateKind::full,"compressed full beats geometrically smaller tail rectangles");
    check(score(p)<4*(408+384),"uses encoded bytes rather than changed area to select compressed full");
}
void sequence_tests() {
    for(auto mode:{msu2::Encoding::raw,msu2::Encoding::pair}) {
        msu2::DeltaPlanner planner;Bytes canvas=random_image(77);
        planner.commit(canvas);std::mt19937 rng(0x565);
        for(unsigned tick=0;tick<90;++tick) {
            auto current=canvas;
            const int x=int(rng()%160),y=int(rng()%80);
            for(int dy=0;dy<1+int(tick%9)&&y+dy<80;++dy)
                for(int dx=0;dx<1+int(tick%13)&&x+dx<160;++dx)invert_pixel(current,x+dx,y+dy);
            if(tick%11==0)current=random_image(100+tick);
            auto p=planner.plan(current,mode);verify(p,canvas,current);
            check(score(p)<=rectangle_score(current,{},mode),"chosen update is no more expensive than full candidate");
            if(tick%4==0)continue; // dropped/aborted source is deliberately not committed
            planner.commit(current);canvas=std::move(current);
        }
    }
}
}
int main() {
    try {
        lifecycle_tests(msu2::Encoding::raw);lifecycle_tests(msu2::Encoding::pair);
        std::cout<<"PASS delta reference/commit/abort/invalidate/invalid input\n";
        geometry_tests();std::cout<<"PASS delta corners/odd rows/wrap/runs/tiles/compressed full/noise\n";
        sequence_tests();std::cout<<"PASS delta deterministic retained-canvas sequence\n";
        std::cout<<"PASS "<<checks<<" delta assertions\n";return 0;
    }catch(const std::exception& e) {
        std::cerr<<"FAIL delta after "<<checks<<" assertions: "<<e.what()<<'\n';return 1;
    }
}
