#pragma once
#include <cstdint>
#include <limits>
namespace msu2app {
// Metadata only. PTS is opaque: its clock domain is NOT assumed to be host time.
struct SourceMetadata {
    std::int64_t epoch=0, sequence=0, callback_ns=0;
    std::int64_t pts_value=0, pts_epoch=0;
    std::int32_t pts_timescale=0;
    std::uint32_t pts_flags=0;
    bool available=false, pts_numeric=false;
};
inline std::int64_t callback_age_us(const SourceMetadata& m,std::int64_t host_ns) {
    if(!m.available||m.callback_ns<=0||host_ns<m.callback_ns)return -1;
    return (host_ns-m.callback_ns)/1000;
}
class SourceReadTracker {
    std::int64_t epoch_=0, sequence_=0;bool seen_=false;
public:
    // -1 unavailable, 0 new source sample, 1 repeated cached sample.
    int observe(const SourceMetadata& m) {
        if(!m.available){seen_=false;return -1;}
        const bool repeated=seen_&&epoch_==m.epoch&&sequence_==m.sequence;
        epoch_=m.epoch;sequence_=m.sequence;seen_=true;return repeated?1:0;
    }
};
}
