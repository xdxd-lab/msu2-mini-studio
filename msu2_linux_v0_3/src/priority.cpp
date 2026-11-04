#include "msu2/priority.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace msu2 {
namespace {
const std::vector<PriorityRegion> empty_regions;
const std::vector<PriorityRegion> bands{
    {"top0",{0,0,160,20}}, {"top1",{0,20,160,20}},
    {"bottom0",{0,40,160,20}}, {"bottom1",{0,60,160,20}}};
const std::vector<PriorityRegion> center{
    {"center",{40,0,80,80}}, {"left",{0,0,40,80}}, {"right",{120,0,40,80}}};
const std::vector<PriorityRegion> halves{
    {"left",{0,0,80,80}}, {"right",{80,0,80,80}}};
}

PriorityMode parse_priority_mode(const std::string& value) {
    if(value=="off") return PriorityMode::off;
    if(value=="bands4") return PriorityMode::bands4;
    if(value=="center3") return PriorityMode::center3;
    if(value=="halves2") return PriorityMode::halves2;
    throw std::invalid_argument("--priority-mode must be off, bands4, center3 or halves2");
}
const char* priority_mode_name(PriorityMode value) {
    switch(value) {
    case PriorityMode::off: return "off";
    case PriorityMode::bands4: return "bands4";
    case PriorityMode::center3: return "center3";
    case PriorityMode::halves2: return "halves2";
    }
    return "unknown";
}
const std::vector<PriorityRegion>& priority_regions(PriorityMode mode) {
    switch(mode) {
    case PriorityMode::off: return empty_regions;
    case PriorityMode::bands4: return bands;
    case PriorityMode::center3: return center;
    case PriorityMode::halves2: return halves;
    }
    throw std::invalid_argument("unknown priority mode");
}
const char* slot_outcome_name(SlotOutcome value) {
    switch(value) {
    case SlotOutcome::sent: return "sent";
    case SlotOutcome::unchanged: return "unchanged";
    case SlotOutcome::skipped: return "skipped";
    case SlotOutcome::failed: return "failed";
    }
    return "unknown";
}

void HostCostEstimator::observe(std::size_t wire_bytes,std::size_t windows,
                                double window_ack_ms,double payload_write_ms) {
    if(!windows || wire_bytes<=windows*18 || !std::isfinite(window_ack_ms) ||
       !std::isfinite(payload_write_ms) || window_ack_ms<0 || payload_write_ms<0)
        throw std::invalid_argument("invalid successful transfer sample");
    const double ack=window_ack_ms/double(windows);
    const double per_byte=payload_write_ms/double(wire_bytes-windows*18);
    if(!ready_) {ack_ms_per_window_=ack;payload_ms_per_byte_=per_byte;ready_=true;}
    else {
        constexpr double alpha=0.25;
        ack_ms_per_window_=(1-alpha)*ack_ms_per_window_+alpha*ack;
        payload_ms_per_byte_=(1-alpha)*payload_ms_per_byte_+alpha*per_byte;
    }
}
HostCostPrediction HostCostEstimator::predict(std::size_t wire_bytes,std::size_t windows,
                                              double already_spent_ms) const {
    if(!std::isfinite(already_spent_ms) || already_spent_ms<0 || wire_bytes<windows*18)
        throw std::invalid_argument("invalid predicted transfer size or preparation time");
    if(!ready_) return {};
    return {true,already_spent_ms+ack_ms_per_window_*double(windows)+
                 payload_ms_per_byte_*double(wire_bytes-windows*18)};
}

PriorityScheduler::PriorityScheduler(PriorityMode requested,unsigned fps)
    :requested_(requested),period_ms_(1000.0/double(fps)),
     safe_needed_(std::max(fps,20u)),bands_counts_(bands.size()),
     center_counts_(center.size()),halves_counts_(halves.size()) {
    if(fps==0 || fps>240) throw std::invalid_argument("priority scheduler fps must be 1..240");
}
std::vector<RegionCounters>& PriorityScheduler::counts(PriorityMode mode) {
    switch(mode) {
    case PriorityMode::bands4: return bands_counts_;
    case PriorityMode::center3: return center_counts_;
    case PriorityMode::halves2: return halves_counts_;
    case PriorityMode::off: break;
    }
    throw std::invalid_argument("off has no region counters");
}
const std::vector<RegionCounters>& PriorityScheduler::counts(PriorityMode mode) const {
    return const_cast<PriorityScheduler*>(this)->counts(mode);
}
const RegionCounters& PriorityScheduler::counters(PriorityMode mode,std::size_t index) const {
    return counts(mode).at(index);
}
void PriorityScheduler::update_age(RegionCounters& count,double now_ms) const {
    const double anchor=count.last_synced_ms<0 ? entered_ms_ : count.last_synced_ms;
    count.max_stale_ms=std::max(count.max_stale_ms,std::max(0.0,now_ms-anchor));
}
void PriorityScheduler::record_missed(PriorityMode mode,std::size_t index,double now_ms) {
    auto& c=counts(mode).at(index);
    ++c.due;++c.missed_queue;++c.consecutive_missed;
    c.max_consecutive_missed=std::max(c.max_consecutive_missed,c.consecutive_missed);
    update_age(c,now_ms);
}
PrioritySelection PriorityScheduler::select(unsigned frame_id,bool reference_valid,
                                            bool predicted_full_valid,double predicted_full_ms,
                                            std::size_t queue_dropped_total,double now_ms,
                                            bool maintenance_full,bool predicted_maintenance_valid,
                                            double predicted_maintenance_ms) {
    if(pending_) throw std::logic_error("complete the previous priority slot first");
    if(!std::isfinite(now_ms) || now_ms<0 ||
       (predicted_full_valid && (!std::isfinite(predicted_full_ms) || predicted_full_ms<0)) ||
       (predicted_maintenance_valid &&
        (!std::isfinite(predicted_maintenance_ms) || predicted_maintenance_ms<0)))
        throw std::invalid_argument("priority timing must be finite and nonnegative");
    if(seen_frame_ && frame_id<=last_frame_id_)
        throw std::invalid_argument("priority frame IDs must be strictly increasing");
    if(seen_frame_ && active_!=PriorityMode::off) {
        const auto count=priority_regions(active_).size();
        for(std::uint64_t id=std::uint64_t(last_frame_id_)+1;id<frame_id;++id)
            record_missed(active_,std::size_t((id-epoch_frame_id_)%count),now_ms);
        for(auto& c:counts(active_)) update_age(c,now_ms);
    }
    const bool new_drop=seen_frame_ && queue_dropped_total>last_dropped_total_;
    last_dropped_total_=queue_dropped_total;
    last_frame_id_=frame_id;
    seen_frame_=true;

    PrioritySelection result;
    result.frame_id=frame_id;
    if(!reference_valid) {
        if(active_!=PriorityMode::off) {active_=PriorityMode::off;++transitions_;result.switched=true;result.trigger="reference_invalid";}
        safe_count_=0;
    } else if(requested_!=PriorityMode::off) {
        if(active_==PriorityMode::off) {
            if(new_drop || (predicted_full_valid && predicted_full_ms>=0.85*period_ms_) ||
               (has_actual_ && last_actual_ms_>=0.95*period_ms_)) {
                active_=requested_;epoch_frame_id_=frame_id;entered_ms_=now_ms;
                safe_count_=0;++transitions_;result.switched=true;
                result.trigger=new_drop ? "queue_drop" :
                    ((predicted_full_valid && predicted_full_ms>=0.85*period_ms_) ? "predicted" : "actual");
            }
        } else {
            const bool safe=predicted_full_valid && predicted_full_ms<=0.60*period_ms_ &&
                has_actual_ && last_actual_ms_<=0.70*period_ms_ && !new_drop &&
                (!maintenance_full || (predicted_maintenance_valid &&
                                       predicted_maintenance_ms<=0.60*period_ms_));
            safe_count_=safe ? safe_count_+1 : 0;
            if(safe_count_>=safe_needed_ && now_ms-entered_ms_>=1000.0) {
                active_=PriorityMode::off;safe_count_=0;++transitions_;
                result.switched=true;result.trigger="sustained_safe";
            }
        }
    }
    result.layout=active_;
    if(active_!=PriorityMode::off) {
        const auto& regions=priority_regions(active_);
        result.partitioned=true;
        result.region_index=(std::uint64_t(frame_id)-epoch_frame_id_)%regions.size();
        result.region=regions[result.region_index];
    }
    pending_=true;
    return result;
}
bool PriorityScheduler::skip_side(const PrioritySelection& selection,
                                  bool predicted_selected_valid,double predicted_selected_ms,
                                  double now_ms) const {
    if(!pending_ || selection.frame_id!=last_frame_id_)
        throw std::logic_error("skip_side requires the current priority slot");
    if(!std::isfinite(now_ms) || now_ms<0 ||
       (predicted_selected_valid && (!std::isfinite(predicted_selected_ms) || predicted_selected_ms<0)))
        throw std::invalid_argument("selected prediction must be finite and nonnegative");
    if(selection.layout!=PriorityMode::center3 || selection.region_index==0) return false;
    if(has_actual_ && last_actual_ms_>=0.90*period_ms_) return true;
    if(!predicted_selected_valid || predicted_selected_ms<0.80*period_ms_) return false;
    // A stale high estimate cannot improve if quiet center slots and skipped
    // sides generate no new transfer samples. Probe a side after a sustained
    // host-idle interval; a genuinely busy preceding slot still blocks it.
    const auto& side=counts(PriorityMode::center3).at(selection.region_index);
    const bool stale=side.last_synced_ms>=0 && now_ms-side.last_synced_ms>=1000.0;
    const bool quiet=has_actual_ && last_actual_ms_<=0.70*period_ms_;
    return !(stale && quiet);
}
void PriorityScheduler::complete(const PrioritySelection& selection,SlotOutcome outcome,
                                 double actual_busy_ms,double now_ms,bool full_refresh) {
    if(!pending_ || selection.frame_id!=last_frame_id_ || selection.layout!=active_)
        throw std::logic_error("priority completion does not match current slot");
    if(!std::isfinite(actual_busy_ms) || actual_busy_ms<0 || !std::isfinite(now_ms) || now_ms<0)
        throw std::invalid_argument("priority timing must be finite and nonnegative");
    if(full_refresh && outcome!=SlotOutcome::sent)
        throw std::invalid_argument("full refresh requires a successful send");
    if(selection.partitioned) {
        auto& c=counts(selection.layout).at(selection.region_index);
        ++c.due;
        switch(outcome) {
        case SlotOutcome::sent: ++c.sent;break;
        case SlotOutcome::unchanged: ++c.unchanged;break;
        case SlotOutcome::skipped: ++c.skipped;break;
        case SlotOutcome::failed: ++c.failed;break;
        }
        if(outcome==SlotOutcome::sent || outcome==SlotOutcome::unchanged) {
            update_age(c,now_ms);c.last_synced_ms=now_ms;
            c.last_synced_frame_id=selection.frame_id;c.consecutive_missed=0;
        } else {
            ++c.consecutive_missed;
            c.max_consecutive_missed=std::max(c.max_consecutive_missed,c.consecutive_missed);
            update_age(c,now_ms);
        }
        if(full_refresh) {
            // The selected slot was serviced by a full-screen keyframe. Its
            // due/sent count remains a single slot, but every region is fresh.
            for(std::size_t i=0;i<counts(selection.layout).size();++i) {
                if(i==selection.region_index) continue;
                auto& other=counts(selection.layout)[i];
                update_age(other,now_ms);
                other.last_synced_ms=now_ms;
                other.last_synced_frame_id=selection.frame_id;
                other.consecutive_missed=0;
            }
        }
    } else if(outcome==SlotOutcome::sent || outcome==SlotOutcome::unchanged) {
        // A completed normal full/diff plan describes the entire current
        // canvas, even if it transmitted only the changed rectangles.
        for(auto mode:{PriorityMode::bands4,PriorityMode::center3,PriorityMode::halves2})
            for(auto& c:counts(mode)) {
                if(c.last_synced_ms>=0) update_age(c,now_ms);
                c.last_synced_ms=now_ms;
                c.last_synced_frame_id=selection.frame_id;
                c.consecutive_missed=0;
            }
    }
    // A no-change or deliberately released slot still consumed CPU time.
    // Refresh the busy observation so an old heavy transfer cannot make all
    // later side slots appear permanently unsafe after the source quiets.
    if(outcome!=SlotOutcome::failed) {last_actual_ms_=actual_busy_ms;has_actual_=true;}
    pending_=false;
}
}
