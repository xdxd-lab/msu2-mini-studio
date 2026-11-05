#pragma once
#include "msu2/protocol.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace msu2 {
enum class UpdateKind { unchanged, full, bounding_box, tiles, runs };

struct Patch {
    Rect rect;
    EncodedFrame frame;
};

struct UpdatePlan {
    UpdateKind kind=UpdateKind::unchanged;
    std::size_t changed_pixels=0;
    std::size_t transmitted_pixels=0;
    // Actual command bytes, including each patch's 18-byte window command.
    // The planner's per-window latency penalty is not included here.
    std::size_t wire_bytes=0;
    std::vector<Patch> patches;
};

const char* update_kind_name(UpdateKind kind);

class DeltaPlanner {
public:
    // Input is always a full 160x80 RGB565 image, high byte first.
    // Planning does not update the reference. Commit only after every patch
    // has been sent successfully, so skipped or failed frames cannot become
    // the basis for later differences.
    UpdatePlan plan(const std::vector<std::uint8_t>& rgb565,
                    Encoding encoding, bool force_full=false) const;
    void commit(const std::vector<std::uint8_t>& rgb565);
    void invalidate();
    bool has_reference() const;

private:
    std::vector<std::uint8_t> reference_;
};
}
