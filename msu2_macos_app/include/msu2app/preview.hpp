#pragma once
#include <cstdint>
#include <vector>
namespace msu2app {
enum class PreviewBorder { dark, light, dual };
struct EdgeContrast { double dark_min=0, light_min=0; PreviewBorder choice=PreviewBorder::dual; };
// Pure, read-only RGB565 edge analysis. Never used by the transport/planner.
EdgeContrast analyze_preview_edges(const std::vector<std::uint8_t>& rgb565);
class BorderStability {
public:
    PreviewBorder update(PreviewBorder desired, double now_ms);
    void reset();
private:
    PreviewBorder current_=PreviewBorder::dual, candidate_=PreviewBorder::dual;
    double since_=-1;
};
}
