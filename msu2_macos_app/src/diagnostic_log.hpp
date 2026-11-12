#pragma once
#include "msu2/trace.hpp"
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace msu2app {
// Opt-in trace (MSU2_DIAGNOSTICS=1, or 2 to add every payload write call).
// record() only appends to a preallocated buffer under a short lock; a writer
// thread formats and flushes every 200 ms, so I/O threads never wait on disk.
// A diagnostics write failure never stops streaming; it is reported instead.
//
// Columns: t_us = integer microseconds (truncated) on steady_clock since this
// log was constructed; frame/patch = -1 when not tied to one. Every *_us field
// below is integer microseconds, *_ms integer milliseconds. Bookkeeping rows:
//   log begin    detail = local wall time of t_us=0 (second resolution)
//   log lost     buffer was full: v0 lost since previous row, v1 lost total,
//                v2/v3 t_us of first/last lost event in that interval
//   log end      v0 event rows written, v1 lost (buffer full), v2 lost after a
//                write failure. Written by finish(), i.e. only when Controller
//                is destroyed normally; a crash or kill leaves no end row, and
//                up to ~200 ms of events plus the warning may then be missing.
// A file without "log,end" must be treated as incomplete.
// Session I/O rows are defined in msu2/trace.hpp; application rows (v0..v5),
// recorded by controller.cpp:
//   session  meta          run, fps, pair(0/1), differential(0/1), chunk_bytes, timeout_ms;
//                          detail = source, encoding, priority, hold_open_ms, auto_reconnect, csv
//   connect  open          begin: reconnect(0/1); ok: reconnect, banner version; failed: reconnect
//   connect  close         session released
//   capture  source        capture_us (Source::get duration), slots_missed_now,
//                          slots_missed_total, queue_replaced_total
//   frame    take          queue_age_us, take_wait_us, id_gap, gaps_total, capture_us
//   frame    pause|stale   dropped before processing (pause change / captured before resume)
//   frame    plan          patches, wire_bytes, changed_pixels, plan_us (RGB565 conversion + plan)
//   frame    replan_full   patches, wire_bytes, changed_pixels after idle renewal
//   patch    rect          x, y, width, height, payload_bytes, pages; result = raw|pair
//   frame    done          frame_us, ack_us, write_us, bytes, patches; result = outcome.
//                          frame_us runs from processing start to completion, excluding an
//                          idle-renewal handshake; ack/write/bytes cover completed patches only
//   renewal  idle          begin: idle_ms; ok: handshake_us, version; failed: handshake_us
//   reconnect attempt      attempt (1..3), delay_ms (begin only)
//   error    ack_timeout|transport_error   classification as counted in Snapshot
// "captured" is the steady_clock time when Source::get returned. For desktop
// capture that is a copy of the latest ScreenCaptureKit frame, not its PTS or
// arrival time, and an idle display can return the same image repeatedly; so
// queue_age_us (captured -> processing start) understates content age.
// take_wait_us runs from just before LatestFrame::take to processing start.
//
// Lifetime: the constructor throws if the file cannot be started, and the
// caller treats that as "diagnostics off". record() may run on any thread. The
// log must outlive every Session holding it as a sink; finish() is called only
// after those users stop, since events recorded later are never written.
class DiagnosticLog final:public msu2::TraceSink{
public:
    struct Status{std::uint64_t written=0,lost_full=0,lost_after_failure=0;bool write_failed=false;std::int64_t failed_at_us=-1;};
    // wall_time fills the "log begin" row; it is not parsed.
    DiagnosticLog(const std::string& path,const std::string& wall_time);
    // Internal dependency injection for deterministic output-failure tests.
    // The stream must outlive this log; only the writer uses it after construction.
    DiagnosticLog(std::ostream& output,const std::string& wall_time);
    ~DiagnosticLog()override;
    // Stops the writer, drains pending events and writes "log,end". Idempotent.
    Status finish();
    Status status()const;
    // Cheap polls for the worker thread; set once and never cleared.
    bool write_failed()const{return failed_.load(std::memory_order_relaxed);}
    bool lost_any()const{return lost_any_.load(std::memory_order_relaxed);}
    void record(const msu2::TraceEvent& e)noexcept override;
private:
    static constexpr std::size_t capacity=16384;
    void start(const std::string& wall_time);
    void run();
    std::uint64_t status_lost_full()const;
    void write(const msu2::TraceEvent& e);
    const std::int64_t origin_=msu2::trace_now_ns();std::ofstream owned_output_;std::ostream& out_;
    mutable std::mutex mutex_;std::condition_variable wake_;bool done_=false;
    std::uint64_t lost_=0;std::int64_t first_lost_ns_=0,last_lost_ns_=0; // guarded by mutex_, reset per interval
    Status status_; // guarded by mutex_
    std::atomic<bool> failed_{false},lost_any_{false};
    std::vector<msu2::TraceEvent> pending_,writing_;std::thread writer_;
};
}
