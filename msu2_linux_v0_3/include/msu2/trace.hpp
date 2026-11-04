#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace msu2 {
// Optional diagnostic event: timing, sizes, indices and result labels. It
// never carries desktop pixels, Pair colours or payload contents.
// kind/stage/result must point to string literals (stored, never copied).
//
// Device input may mix ACKs with echoes or earlier Flash-read data, so rx[]
// holds only bytes that the Session's existing acceptance rule identified:
// the 6-byte MSN banner, the 6-byte MSNCN echo, or the window ACK pattern
// (2-byte prefix 02 03, or the 6-byte echo in echo mode). Anything else is
// reported only as counts (rx_total, match_offset, trailing).
//
// Fields v[0..5] by kind/stage (unused fields are 0; times in microseconds):
//   io  discard_input       call_us; result=bytes_unavailable (the OS flush
//                           reports no count, and tracing never reads it)
//   tx  handshake|window_write|orientation|payload_chunk
//                           len, write_us, offset, chunk_index
//   rx  handshake_banner    wait_us, rx_total, reads, first_byte_us, version, match_offset
//   rx  handshake_echo|window_ack|orientation_ack
//                           wait_us, rx_total, reads, first_byte_us, match_offset, trailing
//       result ok|timeout; on timeout match_offset=-1 and rx[] is empty
//   rxclass <same stage>    rx_total, banner_count, msncn_count, prefix_0203_count,
//                           reads_recorded, reads_not_recorded; result as the rx row.
//                           Follows every rx row. Counts are occurrences, across
//                           read boundaries, in the bytes that wait already read
//                           (including bytes after a match): banner = 00 'M' 'S' 'N'
//                           and two ASCII digits, msncn = 00 'M' 'S' 'N' 'C' 'N',
//                           prefix = 02 03. These patterns cannot overlap one
//                           another; repeated 02 03 runs each count. This is a
//                           classification only, not the ACK acceptance rule;
//                           other bytes are not classified or kept, and the
//                           6-byte echo-mode window ACK is not counted.
//   rxread  <same stage>    read_index, t_us (since the wait began, at read
//                           return), length; result=read. At most 32 per wait
//                           (see reads_not_recorded); omitted when the wait
//                           matched in its only read.
//   payload write           bytes, write_us, chunks, max_chunk_us, max_chunk_index
//   fail <stage>            bytes_done_in_stage, stage_elapsed_us; detail holds the
//                           error message. The stage is where the exception left I/O.
// Application events (capture, frame, patch, renewal, reconnect, error,
// stream) are documented where msu2app records them.
struct TraceEvent {
    std::int64_t t_ns=0; // steady_clock, comparable across threads
    const char* kind="";
    const char* stage="";
    const char* result="";
    std::int64_t frame=-1;
    std::int32_t patch=-1;
    std::array<std::int64_t,6> v{};
    std::uint8_t rx_len=0;
    std::array<std::uint8_t,6> rx{}; // identified pattern only, see above
    std::array<char,96> detail{};
};

// record() is called on I/O threads; it must not block on disk or throw.
class TraceSink {
public:
    virtual ~TraceSink()=default;
    virtual void record(const TraceEvent& event) noexcept=0;
};

inline std::int64_t trace_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline std::int64_t trace_us(std::chrono::steady_clock::duration d) {
    return std::chrono::duration_cast<std::chrono::microseconds>(d).count();
}
inline void trace_detail(TraceEvent& e, std::string_view text) {
    const auto n=std::min(text.size(),e.detail.size()-1);
    for(std::size_t i=0;i<n;++i) e.detail[i]=text[i];
    e.detail[n]='\0';
}
}
