// Deterministic planner corpus. Allocation counts cover C++ operator new calls
// during plan(), not RSS, live memory, malloc calls, or leak detection.
#include "msu2/delta.hpp"
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <new>
#include <string>
#include <vector>
#include <cstdint>
#include <stdexcept>
static bool counting=false;
static std::size_t allocations=0, allocated_bytes=0;
void* operator new(std::size_t n) { if(void* p=std::malloc(n?n:1)){if(counting){++allocations;allocated_bytes+=n;}return p;} throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }
using Bytes=std::vector<std::uint8_t>;
static void number(std::ostream& o, std::uint64_t n) { for(int i=0;i<8;++i)o.put(static_cast<char>(n>>(8*i))); }
static void bytes(std::ostream& o,const std::uint8_t* p,std::size_t n){number(o,n);o.write(reinterpret_cast<const char*>(p),static_cast<std::streamsize>(n));}
static void serialize(std::ostream& o,const msu2::UpdatePlan& p) {
 number(o,static_cast<unsigned>(p.kind));number(o,p.changed_pixels);number(o,p.transmitted_pixels);number(o,p.wire_bytes);number(o,p.patches.size());
 for(const auto& patch:p.patches){const auto& r=patch.rect;number(o,r.x);number(o,r.y);number(o,r.width);number(o,r.height);number(o,patch.frame.raw_bytes);number(o,patch.frame.pages);bytes(o,patch.frame.window.data(),patch.frame.window.size());bytes(o,patch.frame.payload.data(),patch.frame.payload.size());}
}
static void pixel(Bytes& b,int x,int y){auto i=2*(y*160+x);b[i]^=0xff;b[i+1]^=0xff;}
int main(int argc,char** argv){
 if(argc!=3){std::cerr<<"usage: benchmark_delta OUTPUT_PREFIX ROUNDS\n";return 2;}
 try {
  int rounds=std::stoi(argv[2]);if(rounds<1)throw std::runtime_error("rounds must be positive");
  std::string prefix=argv[1];std::ofstream binary(prefix+".plans.bin",std::ios::binary), summary(prefix+".csv");
  if(!binary||!summary)throw std::runtime_error("cannot open output");
  summary<<"case,encoding,force_full,kind,changed_pixels,transmitted_pixels,wire_bytes,rectangles,rounds,total_ns,allocations,allocated_bytes\n";
  Bytes base(25600,0), full=base, noise=base, local=base, sparse=base;
  for(auto& b:full)b=0xff;
  std::uint32_t rng=0x12345678;for(auto& b:noise){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;b=static_cast<std::uint8_t>(rng);}
  for(int y=12;y<25;++y)for(int x=20;x<70;++x)if((x%6<2)||(y==12)||(y==24))pixel(local,x,y);
  for(int y=45;y<55;++y)for(int x=100;x<130;++x)pixel(local,x,y);
  for(int y=0;y<80;y+=13)for(int x=0;x<160;x+=19)pixel(sparse,x,y);
  Bytes corners=base, tiles=base, aligned=base;
  for(int y:{0,79})for(int x:{0,159})pixel(corners,x,y);
  for(int y=0;y<80;y+=8)for(int x=0;x<160;x+=8)pixel(tiles,x,y);
  for(int y=16;y<32;++y)for(int x=32;x<64;++x)pixel(aligned,x,y);
  struct Case{std::string name;const Bytes* reference;const Bytes* data;bool forced;};
  std::vector<Case> cases={{"full",&base,&full,false},{"noise",&base,&noise,false},{"local_text_rect",&base,&local,false},{"sparse",&base,&sparse,false},{"static",&base,&base,false},{"force_full",&base,&local,true},{"corners",&base,&corners,false},{"tile_pixels",&base,&tiles,false},{"aligned_rect",&base,&aligned,false}};
  std::vector<Bytes> sequence;sequence.reserve(21);sequence.push_back(base);
  for(unsigned seed:{17u,31u,77u}) { rng=seed; for(int step=0;step<6;++step){sequence.push_back(sequence.back());for(int j=0;j<40;++j){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;pixel(sequence.back(),rng%160,(rng>>8)%80);}const auto n=sequence.size();cases.push_back({"sequence_"+std::to_string(seed)+"_"+std::to_string(step),&sequence[n-2],&sequence[n-1],false});}}
  for(const auto& c:cases)
   for(auto mode:{msu2::Encoding::raw,msu2::Encoding::pair}){
    msu2::DeltaPlanner planner;planner.commit(*c.reference);auto plan=planner.plan(*c.data,mode,c.forced);
    bytes(binary,reinterpret_cast<const std::uint8_t*>(c.name.data()),c.name.size());number(binary,static_cast<unsigned>(mode));number(binary,c.forced);serialize(binary,plan);
    summary<<c.name<<','<<(mode==msu2::Encoding::raw?"raw":"pair")<<','<<c.forced<<','<<msu2::update_kind_name(plan.kind)<<','<<plan.changed_pixels<<','<<plan.transmitted_pixels<<','<<plan.wire_bytes<<",\"";
    bool first=true;for(const auto& p:plan.patches){if(!first)summary<<';';first=false;summary<<p.rect.x<<':'<<p.rect.y<<':'<<p.rect.width<<':'<<p.rect.height;} summary<<'"';
    allocations=allocated_bytes=0;counting=true;for(int i=0;i<rounds;++i){auto result=planner.plan(*c.data,mode,c.forced);(void)result;}counting=false;auto a=allocations,b=allocated_bytes;
    auto start=std::chrono::steady_clock::now();for(int i=0;i<rounds;++i){auto result=planner.plan(*c.data,mode,c.forced);(void)result;}auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
    summary<<','<<rounds<<','<<ns<<','<<a<<','<<b<<'\n';
   }
  if(!binary||!summary)throw std::runtime_error("output write failed");
 }catch(const std::exception& e){counting=false;std::cerr<<e.what()<<'\n';return 1;}
}
