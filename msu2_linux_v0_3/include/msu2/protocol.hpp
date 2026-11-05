#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
namespace msu2 {
constexpr int screen_width = 160;
constexpr int screen_height = 80;
constexpr std::size_t page_bytes = 256;
using Command = std::array<std::uint8_t, 6>;
inline constexpr Command handshake_command{0, 'M', 'S', 'N', 'C', 'N'};
struct Rect { int x=0, y=0, width=160, height=80; };
enum class Encoding { raw, pair };
struct EncodedFrame {
    std::array<std::uint8_t, 18> window{};
    std::vector<std::uint8_t> payload;
    std::size_t raw_bytes=0;
    std::size_t pages=0;
    std::size_t wire_bytes() const { return window.size()+payload.size(); }
};
void validate_rect(Rect rect);
void encode_frame(const std::vector<std::uint8_t>& rgb565, Rect rect,
                  Encoding encoding, EncodedFrame& output);
Command orientation_command(unsigned value);
}
