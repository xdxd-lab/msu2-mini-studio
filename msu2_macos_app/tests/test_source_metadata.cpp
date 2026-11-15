#include "msu2app/source_metadata.hpp"
#include <cstdlib>
#include <iostream>

namespace {
void check(bool value,const char* message){if(!value){std::cerr<<message<<'\n';std::exit(1);}}
}
int main(){
    using namespace msu2app;
    SourceReadTracker tracker;
    SourceMetadata m;m.available=true;m.epoch=4;m.sequence=8;
    check(tracker.observe(m)==0,"first sample should be new");
    check(tracker.observe(m)==1,"same epoch and sequence should be cached");
    m.epoch=5;check(tracker.observe(m)==0,"new epoch should restart identity");
    m.sequence=9;check(tracker.observe(m)==0,"different sequence is new even with same content");
    m.available=false;check(tracker.observe(m)==-1,"unavailable sample");
    m.available=true;check(tracker.observe(m)==0,"available after gap should be new");

    m.callback_ns=1'000'000;
    check(callback_age_us(m,2'500'999)==1500,"valid callback age");
    m.callback_ns=0;check(callback_age_us(m,2'000'000)==-1,"zero callback timestamp");
    m.callback_ns=-1;check(callback_age_us(m,2'000'000)==-1,"negative callback timestamp");
    m.callback_ns=3'000'000;check(callback_age_us(m,2'000'000)==-1,"future callback timestamp");
    m.callback_ns=1'000'000;m.available=false;check(callback_age_us(m,2'000'000)==-1,"unavailable callback age");

    m.available=true;m.pts_numeric=true;m.pts_value=17;m.pts_timescale=1;m.pts_epoch=2;m.pts_flags=1;
    m.callback_ns=1'000'000;const auto age=callback_age_us(m,9'000'000);
    m.pts_numeric=false;m.pts_value=-99;m.pts_timescale=0;m.pts_epoch=-7;m.pts_flags=0;
    check(callback_age_us(m,9'000'000)==age,"invalid opaque PTS must not affect callback age");
    m.pts_numeric=true;m.pts_value=987654321;m.pts_timescale=600;m.pts_epoch=44;m.pts_flags=0xffffffff;
    check(callback_age_us(m,9'000'000)==age,"different opaque PTS must not affect callback age");
    std::cout<<"Source metadata unit tests passed\n";
}
