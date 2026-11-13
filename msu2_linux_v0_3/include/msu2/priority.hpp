#pragma once

#include "msu2/protocol.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace msu2 {

enum class PriorityMode { off, bands4, center3, halves2 };
PriorityMode parse_priority_mode(const std::string& value);
const char* priority_mode_name(PriorityMode value);

struct PriorityRegion {
    const char* name;
    Rect rect;
};
const std::vector<PriorityRegion>& priority_regions(PriorityMode mode);

enum class SlotOutcome { sent, unchanged, skipped, failed };
const char* slot_outcome_name(SlotOutcome value);

struct RegionCounters {
    std::uint64_t due=0;
    std::uint64_t sent=0;
    std::uint64_t unchanged=0;
    std::uint64_t skipped=0;       // intentional center3 side release
    std::uint64_t missed_queue=0;  // source slot replaced before consumption
    std::uint64_t failed=0;
    std::uint64_t consecutive_missed=0;
    std::uint64_t max_consecutive_missed=0;
    double max_stale_ms=0;
    double last_synced_ms=-1;
    std::int64_t last_synced_frame_id=-1;
};

struct PrioritySelection {
    unsigned frame_id=0;
    PriorityMode layout=PriorityMode::off; // off means normal full/diff planning
    bool partitioned=false;
    bool dirty_override=false; // overdue side chosen at an available dispatch
    PriorityRegion region{"full", {0,0,screen_width,screen_height}};
    std::size_t region_index=0;
    bool switched=false;
    const char* trigger="none";
};

// Predicts host acceptance time only. The prefix ACK and OS writes do not
// establish when LCD pixels become visible.
struct HostCostPrediction {
    bool valid=false;
    double ms=0;
};
class HostCostEstimator {
public:
    void observe(std::size_t wire_bytes, std::size_t windows,
                 double window_ack_ms, double payload_write_ms);
    HostCostPrediction predict(std::size_t wire_bytes, std::size_t windows,
                               double already_spent_ms) const;
private:
    bool ready_=false;
    double ack_ms_per_window_=0;
    double payload_ms_per_byte_=0;
};

// One source frame is considered at a time. The caller must call complete()
// exactly once for every select(), including failed sends. Selection is based
// on source frame IDs, not on successfully transmitted frame counts.
class PriorityScheduler {
public:
    PriorityScheduler(PriorityMode requested, unsigned fps);
    PrioritySelection select(unsigned frame_id, bool reference_valid,
                             bool predicted_full_valid, double predicted_full_ms,
                             std::size_t queue_dropped_total, double now_ms,
                             bool maintenance_full=false,
                             bool predicted_maintenance_valid=false,
                             double predicted_maintenance_ms=0,
                             std::array<bool,2> side_dirty={false,false});
    bool skip_side(const PrioritySelection& selection,
                   bool predicted_selected_valid, double predicted_selected_ms,
                   double now_ms) const;
    void complete(const PrioritySelection& selection, SlotOutcome outcome,
                  double actual_busy_ms, double now_ms, bool full_refresh=false);
    PriorityMode active_layout() const { return active_; }
    unsigned transitions() const { return transitions_; }
    double period_ms() const { return period_ms_; }
    double entered_ms() const { return entered_ms_; }
    const RegionCounters& counters(PriorityMode mode, std::size_t index) const;

private:
    void record_missed(PriorityMode mode, std::size_t index, double now_ms);
    void update_age(RegionCounters& count, double now_ms) const;
    std::vector<RegionCounters>& counts(PriorityMode mode);
    const std::vector<RegionCounters>& counts(PriorityMode mode) const;

    std::array<double,2> side_dirty_since_{{-1,-1}};
    PriorityMode requested_;
    PriorityMode active_=PriorityMode::off;
    double period_ms_;
    unsigned safe_needed_;
    unsigned safe_count_=0;
    unsigned transitions_=0;
    unsigned epoch_frame_id_=0;
    unsigned last_frame_id_=0;
    bool seen_frame_=false;
    bool pending_=false;
    std::size_t last_dropped_total_=0;
    double entered_ms_=0;
    double last_actual_ms_=0;
    bool has_actual_=false;
    std::vector<RegionCounters> bands_counts_;
    std::vector<RegionCounters> center_counts_;
    std::vector<RegionCounters> halves_counts_;
};

}
