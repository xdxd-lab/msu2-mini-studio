#include "msu2/priority.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int checks=0;
void check(bool ok,const std::string& what) {
    ++checks;
    if(!ok)throw std::runtime_error(what);
}
template<class F> void rejects(F&& fn,const std::string& what) {
    bool threw=false;
    try { fn(); } catch(const std::exception&) { threw=true; }
    check(threw,what);
}
using msu2::PriorityMode;
using msu2::PriorityScheduler;
using msu2::SlotOutcome;

void cli_and_geometry() {
    check(msu2::parse_priority_mode("off")==PriorityMode::off,"parse off");
    check(msu2::parse_priority_mode("bands4")==PriorityMode::bands4,"parse bands4");
    check(msu2::parse_priority_mode("center3")==PriorityMode::center3,"parse center3");
    check(msu2::parse_priority_mode("halves2")==PriorityMode::halves2,"parse halves2");
    rejects([]{msu2::parse_priority_mode("Center3");},"priority mode parsing is case-sensitive");
    check(std::string(msu2::priority_mode_name(PriorityMode::center3))=="center3","mode name");
    const auto& bands=msu2::priority_regions(PriorityMode::bands4);
    check(bands.size()==4,"four bands");
    const msu2::Rect expected_bands[]={{0,0,160,20},{0,20,160,20},{0,40,160,20},{0,60,160,20}};
    for(std::size_t i=0;i<bands.size();++i) {
        const auto r=bands[i].rect;
        check(r.x==expected_bands[i].x&&r.y==expected_bands[i].y&&
              r.width==expected_bands[i].width&&r.height==expected_bands[i].height,
              "bands4 row-major partition bounds");
    }
    const auto& center=msu2::priority_regions(PriorityMode::center3);
    check(center.size()==3,"center3 has three regions");
    const msu2::Rect expected_center[]={{40,0,80,80},{0,0,40,80},{120,0,40,80}};
    for(std::size_t i=0;i<center.size();++i) {
        const auto r=center[i].rect;
        check(r.x==expected_center[i].x&&r.y==expected_center[i].y&&
              r.width==expected_center[i].width&&r.height==expected_center[i].height,
              "center3 center/left/right partition order and bounds");
    }
    const auto& halves=msu2::priority_regions(PriorityMode::halves2);
    check(halves.size()==2,"halves2 has two regions");
    const msu2::Rect expected_halves[]={{0,0,80,80},{80,0,80,80}};
    for(std::size_t i=0;i<halves.size();++i) {
        const auto r=halves[i].rect;
        check(r.x==expected_halves[i].x&&r.y==expected_halves[i].y&&
              r.width==expected_halves[i].width&&r.height==expected_halves[i].height,
              "halves2 left/right partition order and bounds");
    }
    check(msu2::priority_regions(PriorityMode::off).empty(),"off has no priority regions");
    rejects([]{PriorityScheduler s(PriorityMode::bands4,0);},"zero fps rejected");
    rejects([]{PriorityScheduler s(PriorityMode::bands4,241);},"fps above 240 rejected");
}

void estimator_tests() {
    msu2::HostCostEstimator estimator;
    auto estimate=estimator.predict(408,1,2.0);
    check(!estimate.valid,"cost prediction is unavailable before a successful sample");
    rejects([&]{estimator.observe(408,0,5,10);},"zero-window cost sample rejected");
    rejects([&]{estimator.observe(17,1,5,10);},"cost sample without payload rejected");
    rejects([&]{estimator.observe(408,1,-1,10);},"negative ACK sample rejected");
    estimator.observe(408,1,5.0,10.0);
    estimate=estimator.predict(408,1,2.0);
    check(estimate.valid&&std::abs(estimate.ms-17.0)<1e-9,
          "prediction includes preparation, per-window ACK, and per-payload-byte costs");
    estimate=estimator.predict(818,2,1.0);
    const double expected=1.0+10.0+10.0*782.0/390.0;
    check(estimate.valid&&std::abs(estimate.ms-expected)<1e-9,
          "prediction scales measured ACK and payload cost across multiple windows");
    rejects([&]{estimator.predict(10,1,0);},"prediction shorter than window overhead rejected");
    rejects([&]{estimator.predict(408,1,-1);},"negative preparation estimate rejected");
}

void entry_triggers_and_default() {
    {
        PriorityScheduler s(PriorityMode::bands4,50);
        auto x=s.select(0,true,true,16.99,0,0);
        check(!x.partitioned&&x.layout==PriorityMode::off&&!x.switched,"below entry threshold remains full/diff mode");
        s.complete(x,SlotOutcome::unchanged,0,0);
        x=s.select(1,true,true,17.0,0,20);
        check(x.partitioned&&x.layout==PriorityMode::bands4&&x.switched&&
              std::string(x.trigger)=="predicted"&&x.region_index==0,
              "predicted full at 85 percent enters and starts phase zero");
        check(s.counters(PriorityMode::bands4,1).last_synced_frame_id==0,
              "successful normal full/unchanged slot marks every region synced");
        s.complete(x,SlotOutcome::sent,10,30);
        check(s.counters(PriorityMode::bands4,0).last_synced_frame_id==1,
              "successful priority region records its source frame as last synced");
    }
    {
        PriorityScheduler s(PriorityMode::halves2,30);
        auto x=s.select(0,true,true,1,0,0);s.complete(x,SlotOutcome::sent,32,32);
        x=s.select(1,true,true,1,0,33);
        check(x.switched&&std::string(x.trigger)=="actual"&&x.region_index==0,
              "last successful busy time at 95 percent enters partitioning");
        s.complete(x,SlotOutcome::sent,5,40);
    }
    {
        PriorityScheduler s(PriorityMode::center3,60);
        auto x=s.select(0,true,false,0,0,0);s.complete(x,SlotOutcome::unchanged,0,1);
        x=s.select(1,true,false,0,1,20);
        check(x.switched&&std::string(x.trigger)=="queue_drop"&&x.region_index==0,
              "new source queue drop enters and starts central phase");
        s.complete(x,SlotOutcome::sent,4,24);
    }
    {
        PriorityScheduler s(PriorityMode::off,30);
        auto x=s.select(0,true,true,100,0,0);
        check(!x.partitioned&&!x.switched&&x.layout==PriorityMode::off,"off mode never partitions");
        s.complete(x,SlotOutcome::sent,100,100);
        x=s.select(1,true,true,100,2,120);
        check(!x.partitioned&&!x.switched,"off mode remains disabled after drops and slow timing");
        s.complete(x,SlotOutcome::unchanged,0,120);
    }
}

void source_ids_and_outcomes() {
    PriorityScheduler s(PriorityMode::bands4,30);
    auto first=s.select(10,true,true,29,0,0);
    check(first.switched&&first.region_index==0,"priority epoch starts on first overloaded source");
    s.complete(first,SlotOutcome::sent,20,20);
    // IDs 11 and 12 were replaced in the latest-frame queue. Their scheduled
    // regions are accounted as missed before source frame 13 is consumed.
    auto fourth=s.select(13,true,true,29,2,100);
    check(fourth.region_index==3&&fourth.region.name==std::string("bottom1"),
          "partition phase follows source frame ids across queue replacements");
    const auto& c1=s.counters(PriorityMode::bands4,1);
    const auto& c2=s.counters(PriorityMode::bands4,2);
    check(c1.due==1&&c1.missed_queue==1&&c1.max_consecutive_missed==1,
          "first replaced source accounts in phase one");
    check(c2.due==1&&c2.missed_queue==1&&c2.max_consecutive_missed==1,
          "second replaced source accounts in phase two");
    check(c1.max_stale_ms>=80&&c2.max_stale_ms>=80,"missed region stale age is tracked");
    s.complete(fourth,SlotOutcome::failed,50,150);
    const auto& c3=s.counters(PriorityMode::bands4,3);
    check(c3.due==1&&c3.failed==1&&c3.sent==0,"failed region slot is distinct from send");
    auto next=s.select(14,true,true,29,2,160);
    check(next.region_index==0,"failed send consumes its scheduled source slot");
    s.complete(next,SlotOutcome::unchanged,0,170);
    const auto& c0=s.counters(PriorityMode::bands4,0);
    check(c0.due==2&&c0.sent==1&&c0.unchanged==1,"sent and unchanged outcomes are separate counters");
    check(c3.consecutive_missed==1&&c0.consecutive_missed==0,
          "successful unchanged sync resets consecutive misses only for its region");
    check(c3.last_synced_frame_id==-1&&c0.last_synced_frame_id==14,
          "failed region keeps old sync age while unchanged region advances source frame");
}

void required_completion_and_validation() {
    PriorityScheduler s(PriorityMode::halves2,30);
    auto x=s.select(0,true,true,29,0,0);
    rejects([&]{s.select(1,true,true,29,0,1);},"every selection requires one completion");
    auto other=x;other.frame_id=77;
    rejects([&]{s.complete(other,SlotOutcome::failed,0,1);},"completion must match selection");
    rejects([&]{s.skip_side(x,true,-1,1);},"negative selected estimate rejected");
    s.complete(x,SlotOutcome::failed,0,1);
    rejects([&]{s.complete(x,SlotOutcome::failed,0,1);},"duplicate completion rejected");
    rejects([&]{s.select(0,true,true,29,0,2);},"source frame IDs must increase");
    rejects([&]{s.select(1,true,true,std::nan(""),0,2);},"nonfinite full estimate rejected");
}

void center_skips_and_cadence() {
    PriorityScheduler s(PriorityMode::center3,30);
    auto center=s.select(0,true,true,29,0,0);
    check(center.region_index==0&&center.region.name==std::string("center"),"center3 begins with center");
    check(!s.skip_side(center,true,100,1),"center slot is never skipped");
    s.complete(center,SlotOutcome::sent,31,31);
    auto left=s.select(1,true,true,29,0,30);
    check(left.region_index==1&&left.region.name==std::string("left"),"center3 second slot is left side");
    check(s.skip_side(left,true,27,40),"side skipped at 80 percent predicted selected time");
    s.complete(left,SlotOutcome::skipped,31,60);
    auto right=s.select(2,true,true,29,0,70);
    check(right.region_index==2&&right.region.name==std::string("right"),"skipping left preserves source-id cadence to right");
    check(s.skip_side(right,false,0,80),"side skipped when previous actual busy time reaches 90 percent");
    s.complete(right,SlotOutcome::skipped,5,100);
    auto next_center=s.select(3,true,true,29,0,110);
    check(next_center.region_index==0&&next_center.region.name==std::string("center"),"side skips never shift the next central slot");
    check(!s.skip_side(next_center,true,100,121),"central cadence remains protected under side risk");
    s.complete(next_center,SlotOutcome::unchanged,0,120);
    auto next_left=s.select(4,true,true,29,0,130);
    check(!s.skip_side(next_left,true,5,131),"quiet center/skip observations release a side slot when its estimate is safe");
    s.complete(next_left,SlotOutcome::unchanged,1,140);
    const auto& c=s.counters(PriorityMode::center3,0);
    check(c.due==2&&c.sent==1&&c.unchanged==1&&c.skipped==0,"center slots are kept and separately counted");
    check(s.counters(PriorityMode::center3,1).skipped==1&&s.counters(PriorityMode::center3,1).unchanged==1&&
          s.counters(PriorityMode::center3,2).skipped==1,"released side slots have separate counts");
    PriorityScheduler other(PriorityMode::bands4,30);
    auto b=other.select(0,true,true,29,0,0);
    check(!other.skip_side(b,true,100,1),"bands4 scheduled slots are never intentionally skipped");
    other.complete(b,SlotOutcome::failed,0,1);
}

void hysteresis_and_reference_invalidation() {
    constexpr unsigned fps=10;
    PriorityScheduler s(PriorityMode::halves2,fps);
    auto x=s.select(0,true,true,86,0,0); // enter at 86% of a 100ms period
    check(x.switched&&x.layout==PriorityMode::halves2,"high predicted full duration enters partitioning");
    s.complete(x,SlotOutcome::sent,20,20);
    unsigned id=1;
    // 19 safe opportunities and more than the dwell interval are insufficient.
    for(unsigned i=0;i<19;++i,++id) {
        x=s.select(id,true,true,60,0,double(id)*60.0);
        check(!x.switched&&x.layout==PriorityMode::halves2,"partition stays active before required safe streak");
        s.complete(x,SlotOutcome::unchanged,0,double(id)*60.0+1);
    }
    check(double(id-1)*60.0>=1000.0,"test crossed the minimum dwell interval");
    x=s.select(id,true,true,60,0,double(id)*60.0);
    check(x.switched&&x.layout==PriorityMode::off&&std::string(x.trigger)=="sustained_safe",
          "twentieth safe opportunity exits after dwell");
    s.complete(x,SlotOutcome::unchanged,0,double(id)*60.0+1);
    check(s.transitions()==2,"entry and exit count as two transitions");

    PriorityScheduler reset(PriorityMode::bands4,fps);
    x=reset.select(0,true,true,86,0,0);reset.complete(x,SlotOutcome::sent,20,1);
    for(unsigned i=1;i<19;++i) {
        x=reset.select(i,true,true,60,0,double(i)*60);reset.complete(x,SlotOutcome::unchanged,0,double(i)*60+1);
    }
    x=reset.select(19,true,true,60,1,1200); // a new drop resets the safe streak
    check(!x.switched&&x.layout==PriorityMode::bands4,"new queue drop cancels safe exit streak");
    reset.complete(x,SlotOutcome::unchanged,0,1201);
    for(unsigned i=20;i<39;++i) {
        x=reset.select(i,true,true,60,1,1200+double(i-19)*60);
        check(!x.switched,"safe streak restarts after queue drop");
        reset.complete(x,SlotOutcome::unchanged,0,1201+double(i-19)*60);
    }
    x=reset.select(39,true,true,60,1,2400);
    check(x.switched&&x.layout==PriorityMode::off,"scheduler exits after a fresh full safe streak");
    reset.complete(x,SlotOutcome::unchanged,0,2461);

    PriorityScheduler invalid(PriorityMode::center3,30);
    x=invalid.select(0,true,true,29,0,0);invalid.complete(x,SlotOutcome::sent,10,10);
    x=invalid.select(1,false,true,29,0,20);
    check(x.switched&&x.layout==PriorityMode::off&&std::string(x.trigger)=="reference_invalid",
          "lost retained display reference immediately disables partition mode");
    invalid.complete(x,SlotOutcome::failed,0,21);
}

void maintenance_full_semantics() {
    constexpr unsigned fps=10;
    PriorityScheduler s(PriorityMode::bands4,fps);
    auto x=s.select(0,true,true,1,0,0);
    check(!x.partitioned,"maintenance test begins with baseline full mode");
    s.complete(x,SlotOutcome::sent,10,1);
    x=s.select(1,true,true,86,0,100);
    check(x.switched&&x.partitioned&&x.region_index==0,"maintenance test enters banded mode");
    s.complete(x,SlotOutcome::sent,10,101);
    for(unsigned id=2;id<=20;++id) {
        x=s.select(id,true,true,60,0,100.0+double(id-1)*60.0);
        check(x.partitioned&&!x.switched,"safe opportunities do not leave before maintenance");
        s.complete(x,SlotOutcome::unchanged,10,101.0+double(id-1)*60.0);
    }
    const auto before0=s.counters(PriorityMode::bands4,0);
    const auto before1=s.counters(PriorityMode::bands4,1);
    const auto before2=s.counters(PriorityMode::bands4,2);
    const auto before3=s.counters(PriorityMode::bands4,3);
    x=s.select(21,true,true,60,0,1300,true,true,90);
    check(x.partitioned&&!x.switched&&x.region_index==0,
          "due full maintenance suppresses an otherwise-ready adaptive exit");
    s.complete(x,SlotOutcome::sent,10,1301,true);
    check(s.active_layout()==PriorityMode::bands4&&s.transitions()==1,
          "successful maintenance keyframe keeps partition mode active");
    const auto after0=s.counters(PriorityMode::bands4,0);
    const auto after1=s.counters(PriorityMode::bands4,1);
    const auto after2=s.counters(PriorityMode::bands4,2);
    const auto after3=s.counters(PriorityMode::bands4,3);
    check(after0.due==before0.due+1&&after0.sent==before0.sent+1,
          "maintenance full counts the current scheduled slot once");
    check(after1.due==before1.due&&after1.sent==before1.sent&&
          after2.due==before2.due&&after2.sent==before2.sent&&
          after3.due==before3.due&&after3.sent==before3.sent,
          "maintenance full does not invent due or sent work for other slots");
    for(std::size_t i=0;i<4;++i)
        check(s.counters(PriorityMode::bands4,i).last_synced_frame_id==21,
              "successful maintenance full refreshes every region source ID");
    for(unsigned id=22;id<=40;++id) {
        x=s.select(id,true,true,60,0,1360.0+double(id-22)*60.0,false);
        check(!x.switched&&x.partitioned,
              "an expensive maintenance keyframe resets rather than advances the safe streak");
        s.complete(x,SlotOutcome::unchanged,10,1361.0+double(id-22)*60.0);
    }
    x=s.select(41,true,true,60,0,2500,false);
    check(x.switched&&!x.partitioned&&x.layout==PriorityMode::off&&
          std::string(x.trigger)=="sustained_safe",
          "continued safe conditions allow normal exit after a fresh full safe streak");
    s.complete(x,SlotOutcome::unchanged,0,2501);

    PriorityScheduler every_slot_keyframe(PriorityMode::halves2,fps);
    x=every_slot_keyframe.select(0,true,true,1,0,0);
    every_slot_keyframe.complete(x,SlotOutcome::sent,10,1);
    x=every_slot_keyframe.select(1,true,true,86,0,100);
    check(x.switched&&x.partitioned,"per-slot keyframe test enters adaptive mode");
    every_slot_keyframe.complete(x,SlotOutcome::sent,10,101);
    for(unsigned id=2;id<=20;++id) {
        x=every_slot_keyframe.select(id,true,true,60,0,100.0+double(id-1)*60.0,
                                     true,true,60);
        check(!x.switched&&x.partitioned,"safe full maintenance can accumulate exit hysteresis");
        every_slot_keyframe.complete(x,SlotOutcome::sent,10,101.0+double(id-1)*60.0,true);
    }
    x=every_slot_keyframe.select(21,true,true,60,0,1300,true,true,60);
    check(x.switched&&!x.partitioned&&std::string(x.trigger)=="sustained_safe",
          "when every due keyframe is predicted safe, the scheduler can exit after the normal dwell");
    every_slot_keyframe.complete(x,SlotOutcome::sent,10,1301);

    PriorityScheduler failed(PriorityMode::halves2,30);
    x=failed.select(0,true,true,1,0,0);
    failed.complete(x,SlotOutcome::sent,5,10);
    x=failed.select(1,true,true,29,0,20);
    check(x.switched&&x.partitioned,"failed-maintenance test enters adaptive mode");
    failed.complete(x,SlotOutcome::sent,5,30);
    x=failed.select(2,true,true,29,0,40,true);
    const auto peer0=failed.counters(PriorityMode::halves2,0).last_synced_frame_id;
    const auto peer1=failed.counters(PriorityMode::halves2,1).last_synced_frame_id;
    failed.complete(x,SlotOutcome::failed,5,50,false);
    check(failed.active_layout()==PriorityMode::halves2&&failed.transitions()==1,
          "failed maintenance send does not force an adaptive exit");
    check(failed.counters(PriorityMode::halves2,x.region_index).failed==1&&
          failed.counters(PriorityMode::halves2,x.region_index).sent==0,
          "failed maintenance consumes only a failed current slot, not a successful send");
    check(failed.counters(PriorityMode::halves2,0).last_synced_frame_id==peer0&&
          failed.counters(PriorityMode::halves2,1).last_synced_frame_id==peer1,
          "failed maintenance does not advance any full-screen sync reference");
}

void bounded_center_side_probe() {
    PriorityScheduler s(PriorityMode::center3,30);
    auto x=s.select(0,true,true,1,0,0);
    s.complete(x,SlotOutcome::sent,100,100); // one slow full ACK observed
    x=s.select(1,true,true,29,0,120);
    check(x.switched&&x.region_index==0,"slow full transfer enters center3 on the next source frame");
    s.complete(x,SlotOutcome::unchanged,1,121); // the center is stable; busy time has recovered

    unsigned id=2;
    unsigned skipped[2]={0,0};
    unsigned probed[2]={0,0};
    while(id<110) {
        const double now=121.0+double(id-1)*34.0;
        x=s.select(id,true,true,29,0,now);
        if(x.region_index==1 || x.region_index==2) {
            const std::size_t side=x.region_index-1;
            const bool skip=s.skip_side(x,true,40.0,now); // estimator still predicts a slow side transfer
            if(skip) ++skipped[side];
            const double last_sync=s.counters(PriorityMode::center3,x.region_index).last_synced_ms;
            if(now-last_sync>=1000.0) {
                check(!skip,"a side receives a probe when its own sync age reaches one second");
                s.complete(x,SlotOutcome::sent,2,now+2);
                ++probed[side];
            } else {
                check(skip,"high estimate continues to release a recently synchronized side");
                s.complete(x,skip?SlotOutcome::skipped:SlotOutcome::unchanged,1,now+1);
            }
        } else {
            check(!s.skip_side(x,true,40.0,now),"stable central slots remain protected during side probing");
            s.complete(x,SlotOutcome::unchanged,1,now+1);
        }
        ++id;
    }
    check(id==110,"bounded side probes finish within the deterministic test horizon");
    check(skipped[0]>0&&skipped[1]>0,"high estimates first cause deliberate releases for both sides");
    for(std::size_t side=0;side<2;++side) {
        const auto& count=s.counters(PriorityMode::center3,side+1);
        check(probed[side]>0&&count.sent>0&&count.skipped==skipped[side],
              "each stale side is probed and retains its intentional-skip telemetry");
        check(count.last_synced_frame_id>=30,
              "each stale side records a newer source frame after its bounded probe");
    }
}

// Virtual times keep the dirty deadline regression independent of host load.
void dirty_side_dispatch_deadline() {
    auto pick=[](PriorityScheduler& scheduler,unsigned id,double now,
                 std::array<bool,2> dirty=std::array<bool,2>{{false,false}}) {
        return scheduler.select(id,true,true,90,0,now,false,false,0,dirty);
    };
    auto done=[](PriorityScheduler& scheduler,msu2::PrioritySelection selection,
                 double now,SlotOutcome outcome=SlotOutcome::sent,bool full=false) {
        scheduler.complete(selection,outcome,20,now,full);
    };

 // Missing all nominal side slots cannot indefinitely suppress dispatch.
 PriorityScheduler s(PriorityMode::center3,60);
 auto x=pick(s,0,0,{true,true});done(s,x,1);
 x=pick(s,3,999,{true,true});check(x.region_index==0,"no early override");done(s,x,999);
 x=pick(s,6,1000,{true,true});check(x.region_index==1,"dirty deadline must dispatch left despite modulo gaps");
 check(!s.skip_side(x,true,90,1000),"overdue bypasses busy skip");done(s,x,1001);
 x=pick(s,9,1002,{true,true});check(x.region_index==2,"right oldest after left success");done(s,x,1003);
 x=pick(s,12,1004,{false,false});check(x.region_index==0,"static sides not forced");done(s,x,1005,SlotOutcome::unchanged);
 check(s.counters(PriorityMode::center3,1).sent==1 && s.counters(PriorityMode::center3,2).sent==1,"unchanged not sent");
 // Difference disappears before deadline: a new difference starts a new wait.
 PriorityScheduler r(PriorityMode::center3,60);
 x=pick(r,0,0,{true,false});done(r,x,1);
 x=pick(r,3,900,{false,false});done(r,x,901,SlotOutcome::unchanged);
 x=pick(r,6,1000,{true,false});check(x.region_index==0,"new dirty starts fresh");done(r,x,1001);
 x=pick(r,9,1999,{true,false});check(x.region_index==0,"persistent changes do not expire early");done(r,x,1999);
 x=pick(r,12,2000,{true,false});check(x.region_index==1,"persistent dirty expires without restart");done(r,x,2001);
 // A full send covers both sides and clears prior pending dirtiness.
 PriorityScheduler f(PriorityMode::center3,60);
 x=pick(f,0,0,{true,true});done(f,x,1);
 x=pick(f,3,900,{true,true});done(f,x,901,SlotOutcome::sent,true);
 x=pick(f,6,1000,{true,true});check(x.region_index==0,"full clears both timers");done(f,x,1001);
 x=pick(f,9,1999,{true,true});check(x.region_index==0,"full reset no early expiry");done(f,x,1999);
 // No side differences, even at late times and a busy prior transfer.
 PriorityScheduler quiet(PriorityMode::center3,60);
 for(unsigned i=0;i<8;++i){x=pick(quiet,i*3,i*2000);check(x.region_index==0,"static sides never override");done(quiet,x,i*2000+1,SlotOutcome::unchanged);}
}
}

int main() {
    try {
        cli_and_geometry();
        estimator_tests();
        entry_triggers_and_default();
        source_ids_and_outcomes();
        required_completion_and_validation();
        center_skips_and_cadence();
        hysteresis_and_reference_invalidation();
        maintenance_full_semantics();
        bounded_center_side_probe();
        dirty_side_dispatch_deadline();
        std::cout<<"PASS "<<checks<<" priority scheduler assertions\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr<<"FAIL priority scheduler after "<<checks<<" assertions: "<<e.what()<<'\n';
        return 1;
    }
}
