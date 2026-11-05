#include "msu2/delta.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace msu2 {
namespace {
constexpr std::size_t frame_pixels=
    std::size_t(screen_width)*std::size_t(screen_height);
constexpr std::size_t frame_bytes=frame_pixels*2;
constexpr std::size_t maximum_patches=32;
constexpr int tile_size=8;
constexpr int tile_columns=(screen_width+tile_size-1)/tile_size;
constexpr int tile_rows=(screen_height+tile_size-1)/tile_size;

// Approximate the fixed cost of opening a window and waiting for its ACK.
// This is a selection heuristic, not an extra command or a measured latency.
// A few unchanged pixels can cost less than an additional small transaction.
constexpr std::size_t window_penalty_bytes=384;

void validate_input(const std::vector<std::uint8_t>& pixels) {
    if(pixels.size()!=frame_bytes)
        throw std::invalid_argument("delta input must be a full MINI 160x80 RGB565 image");
}

bool same_rect(const Rect& a,const Rect& b) {
    return a.x==b.x && a.y==b.y && a.width==b.width && a.height==b.height;
}

bool is_full_screen(const Rect& rect) {
    return same_rect(rect,Rect{0,0,screen_width,screen_height});
}

// Rectangle sequences already encoded by one plan() call, in fixed storage so
// the bookkeeping adds no heap allocation. Encoding is deterministic, so a
// repeated sequence has the same bytes and score as its earlier candidate and
// can neither beat nor tie-break it; skipping it keeps the plan identical.
// Recording is best effort: a sequence that does not fit is merely encoded again.
class EncodedGeometries {
public:
    bool contains(const std::vector<Rect>& rectangles) const {
        for(std::size_t i=0;i<count_;++i)
            if(size_[i]==rectangles.size() &&
               std::equal(rectangles.begin(),rectangles.end(),
                          rects_.begin()+std::ptrdiff_t(begin_[i]),same_rect))
                return true;
        return false;
    }
    void add(const std::vector<Rect>& rectangles) {
        if(count_==size_.size() || rectangles.size()>rects_.size()-used_) return;
        std::copy(rectangles.begin(),rectangles.end(),rects_.begin()+std::ptrdiff_t(used_));
        begin_[count_]=used_;
        size_[count_]=rectangles.size();
        used_+=rectangles.size();
        ++count_;
    }
private:
    // Enough for full, bounding box and one capped tile or run sequence.
    std::array<Rect,2+maximum_patches> rects_{};
    std::array<std::size_t,4> begin_{}, size_{};
    std::size_t count_=0, used_=0;
};

UpdatePlan encode_candidate(const std::vector<std::uint8_t>& pixels,
                            const std::vector<Rect>& rectangles,
                            Encoding encoding, UpdateKind kind,
                            std::size_t changed_pixels) {
    UpdatePlan result;
    result.kind=kind;
    result.changed_pixels=changed_pixels;
    result.patches.reserve(rectangles.size());
    std::vector<std::uint8_t> cropped;
    for(const auto& rect:rectangles) {
        Patch patch;
        patch.rect=rect;
        if(is_full_screen(rect)) {
            // The full-screen crop is the input itself; encode it without a copy.
            encode_frame(pixels,rect,encoding,patch.frame);
        } else {
            const auto row_bytes=std::size_t(rect.width)*2;
            cropped.resize(row_bytes*std::size_t(rect.height));
            for(int y=0;y<rect.height;++y) {
                const auto source_offset=
                    (std::size_t(rect.y+y)*screen_width+std::size_t(rect.x))*2;
                std::copy_n(pixels.data()+source_offset,row_bytes,
                            cropped.data()+std::size_t(y)*row_bytes);
            }
            encode_frame(cropped,rect,encoding,patch.frame);
        }
        result.wire_bytes+=patch.frame.wire_bytes();
        result.transmitted_pixels+=std::size_t(rect.width)*std::size_t(rect.height);
        result.patches.push_back(std::move(patch));
    }
    return result;
}

std::size_t score(const UpdatePlan& plan) {
    return plan.wire_bytes+window_penalty_bytes*plan.patches.size();
}

// Partition marked cells into horizontal runs. A run can extend a rectangle
// from the immediately previous row only when x and width match exactly.
// Coordinates are scaled for the 8x8 tile candidate; pixel runs use scale=1.
// Previously ended rectangles never merge later, so exceeding the cap here
// guarantees that the final candidate would also exceed the cap.
template<std::size_t N>
bool run_rectangles(const std::array<std::uint8_t,N>& marked,
                    int columns, int rows, int scale,
                    std::vector<Rect>& rectangles) {
    rectangles.clear();
    rectangles.reserve(maximum_patches);
    std::vector<std::size_t> previous;
    std::vector<std::size_t> current;
    previous.reserve(maximum_patches);
    current.reserve(maximum_patches);
    for(int row=0;row<rows;++row) {
        current.clear();
        int column=0;
        while(column<columns) {
            if(!marked[std::size_t(row)*columns+column]) {
                ++column;
                continue;
            }
            const int start=column;
            do { ++column; }
            while(column<columns && marked[std::size_t(row)*columns+column]);
            const int x=start*scale;
            const int y=row*scale;
            const int width=std::min(column*scale,screen_width)-x;
            const int height=std::min(scale,screen_height-y);
            auto match=previous.end();
            for(auto it=previous.begin();it!=previous.end();++it) {
                const auto& prior=rectangles[*it];
                if(prior.x==x && prior.width==width && prior.y+prior.height==y) {
                    match=it;
                    break;
                }
            }
            if(match!=previous.end()) {
                rectangles[*match].height+=height;
                current.push_back(*match);
            } else {
                if(rectangles.size()==maximum_patches) return false;
                rectangles.push_back({x,y,width,height});
                current.push_back(rectangles.size()-1);
            }
        }
        previous.swap(current);
    }
    return true;
}
}

const char* update_kind_name(UpdateKind kind) {
    switch(kind) {
    case UpdateKind::unchanged: return "unchanged";
    case UpdateKind::full: return "full";
    case UpdateKind::bounding_box: return "bounding_box";
    case UpdateKind::tiles: return "tiles";
    case UpdateKind::runs: return "runs";
    }
    return "unknown";
}

UpdatePlan DeltaPlanner::plan(const std::vector<std::uint8_t>& pixels,
                              Encoding encoding, bool force_full) const {
    validate_input(pixels);
    // Validate even an unchanged frame, which otherwise never calls encoder.
    if(encoding!=Encoding::raw && encoding!=Encoding::pair)
        throw std::invalid_argument("unknown encoding");

    const std::vector<Rect> full{{0,0,screen_width,screen_height}};
    if(!has_reference())
        return encode_candidate(pixels,full,encoding,UpdateKind::full,frame_pixels);

    std::array<std::uint8_t,frame_pixels> changed{};
    std::array<std::uint8_t,std::size_t(tile_columns)*tile_rows> dirty_tiles{};
    std::size_t changed_pixels=0;
    int min_x=screen_width, min_y=screen_height, max_x=-1, max_y=-1;
    for(int y=0;y<screen_height;++y) {
        for(int x=0;x<screen_width;++x) {
            const auto pixel=std::size_t(y)*screen_width+std::size_t(x);
            const auto byte=pixel*2;
            if(pixels[byte]==reference_[byte] && pixels[byte+1]==reference_[byte+1])
                continue;
            changed[pixel]=1;
            dirty_tiles[std::size_t(y/tile_size)*tile_columns+x/tile_size]=1;
            ++changed_pixels;
            min_x=std::min(min_x,x); min_y=std::min(min_y,y);
            max_x=std::max(max_x,x); max_y=std::max(max_y,y);
        }
    }
    if(!changed_pixels && !force_full) return {};

    auto best=encode_candidate(pixels,full,encoding,UpdateKind::full,changed_pixels);
    if(force_full) return best;

    // Stable preference on a complete tie: full, bounding box, tiles, runs.
    // Actual encoded sizes include page padding and page-local compression.
    // This avoids assuming that a smaller pixel rectangle always costs less.
    // Full-screen motion often makes bounding and tiles equal to full; a
    // repeated geometry keeps the earlier kind, as the tie rule already would.
    EncodedGeometries encoded;
    encoded.add(full);
    const auto consider=[&](const std::vector<Rect>& rectangles,UpdateKind kind) {
        if(encoded.contains(rectangles)) return;
        auto candidate=encode_candidate(pixels,rectangles,encoding,kind,changed_pixels);
        encoded.add(rectangles);
        if(score(candidate)<score(best) ||
           (score(candidate)==score(best) && candidate.patches.size()<best.patches.size()))
            best=std::move(candidate);
    };
    const std::vector<Rect> bounding{{min_x,min_y,max_x-min_x+1,max_y-min_y+1}};
    consider(bounding,UpdateKind::bounding_box);
    std::vector<Rect> rectangles;
    if(run_rectangles(dirty_tiles,tile_columns,tile_rows,tile_size,rectangles))
        consider(rectangles,UpdateKind::tiles);
    if(run_rectangles(changed,screen_width,screen_height,1,rectangles))
        consider(rectangles,UpdateKind::runs);
    return best;
}

void DeltaPlanner::commit(const std::vector<std::uint8_t>& pixels) {
    validate_input(pixels);
    reference_=pixels;
}

void DeltaPlanner::invalidate() { reference_.clear(); }

bool DeltaPlanner::has_reference() const { return !reference_.empty(); }
}
