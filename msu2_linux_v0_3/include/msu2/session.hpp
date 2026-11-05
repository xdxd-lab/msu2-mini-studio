#pragma once
#include "msu2/protocol.hpp"
#include "msu2/trace.hpp"
#include "msu2/transport.hpp"
#include <string>
#include <thread>
namespace msu2 {
enum class AckMode { prefix, echo };
struct SessionOptions {
    Milliseconds handshake_timeout{5000};
    Milliseconds io_timeout{2000};
    std::size_t write_chunk=3840; // Core default; must be a multiple of 6. The macOS App selects 60.
    AckMode ack_mode=AckMode::prefix;
    // Optional, owned by the caller and must outlive the Session. Tracing only
    // observes: the transport call sequence, arguments and timeouts are the
    // same with or without it.
    TraceSink* trace=nullptr;
    bool trace_chunks=false; // also record every payload write call
};
struct TransferStats {
    double window_ack_ms=0;
    double payload_write_ms=0;
    std::size_t bytes=0;
    std::size_t write_calls=0;
};
// Not thread-safe: create and use on one thread. I/O methods enforce the
// construction thread; observers and trace-context setters need the same ownership.
class Session {
public:
    explicit Session(Transport&, SessionOptions = {});
    int handshake(); // wait for 00 MSN + two ASCII digits, then exact MSNCN echo
    void set_orientation(unsigned value);
    TransferStats send(const EncodedFrame& frame);
    const std::vector<std::uint8_t>& last_response() const { return last_response_; }
    bool connected() const { return connected_; }
    // Diagnostic labels for following events only; no effect on I/O.
    void set_trace_context(std::int64_t frame, int patch) { trace_frame_=frame; trace_patch_=patch; }
private:
    void require_owner() const;
    void wait_pattern(const std::vector<std::uint8_t>& pattern, Milliseconds timeout, const char* stage);
    void await_lcd(const Command& cmd, const char* stage);
    TraceEvent trace_event(const char* kind, const char* stage, const char* result) const;
    void trace_tx(const char* stage, std::size_t length, std::int64_t write_us,
                  std::size_t offset=0, std::size_t index=0) const;
    void trace_failure(const char* stage, std::size_t done, std::int64_t elapsed_us) const;
    Transport& transport_;
    SessionOptions options_;
    std::thread::id owner_;
    bool connected_=false;
    std::vector<std::uint8_t> last_response_;
    std::int64_t trace_frame_=-1;
    int trace_patch_=-1;
};
}
