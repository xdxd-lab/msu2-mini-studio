// Offline regression for rejected requests (MSU2-07); no device or screen access.
// A request rejected while the Controller is busy or closing must keep the
// existing rejection semantics and leave a trace that a later log line cannot
// overwrite: one events.log line, and one request/rejected diagnostics row when
// diagnostics are enabled.
#include "msu2app/controller.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
using namespace std::chrono_literals;
namespace {
const char* kReject = "請先停止目前工作，再執行新操作";
const char* kPort = "/dev/offline-reject";
void check(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
template<class F> void wait_until(F pred, const char* what) {
    const auto end = std::chrono::steady_clock::now() + 5s;
    while (!pred()) { if (std::chrono::steady_clock::now() > end) throw std::runtime_error(std::string("timed out: ") + what); std::this_thread::sleep_for(1ms); }
}
struct Gate {
    std::mutex m; std::condition_variable cv; bool held = false, entered = false;
    void arm() { std::lock_guard<std::mutex> l(m); held = true; entered = false; }
    void block() { std::unique_lock<std::mutex> l(m); if (!held) return; entered = true; cv.notify_all(); cv.wait(l, [&] { return !held; }); }
    void await() { std::unique_lock<std::mutex> l(m); check(cv.wait_for(l, 5s, [&] { return entered; }), "gate not entered"); }
    void release() { std::lock_guard<std::mutex> l(m); held = false; cv.notify_all(); }
};
struct Shared { Gate destroy, handshake; std::atomic<unsigned> opens{0}, leases{0}; };
struct Fake final : msu2::Transport {
    std::shared_ptr<Shared> s; std::deque<std::uint8_t> rx{0, 'M', 'S', 'N', '0', '1'};
    explicit Fake(std::shared_ptr<Shared> x) : s(std::move(x)) { ++s->opens; }
    ~Fake() override { s->destroy.block(); }
    std::size_t read(std::uint8_t* dst, std::size_t cap, msu2::Milliseconds) override {
        auto n = std::min(cap, rx.size()); for (std::size_t i = 0; i < n; ++i) { dst[i] = rx.front(); rx.pop_front(); } return n;
    }
    void discard_input() override { rx.clear(); }
    void write_all(const std::uint8_t* p, std::size_t n, msu2::Milliseconds) override {
        if (n == 6 && p[0] == 0) { s->handshake.block(); rx.insert(rx.end(), p, p + n); }
        else if (n == 18 && p[0] == 2 && p[1] == 0) rx.insert(rx.end(), {2, 3, 7, 0, 0, 0});
    }
};
// Releases gates before the Controller is destroyed if an assertion throws.
struct ReleaseGates { std::shared_ptr<Shared> s; ~ReleaseGates() { s->destroy.release(); s->handshake.release(); } };
msu2app::Dependencies dependencies(const std::shared_ptr<Shared>& s) {
    msu2app::Dependencies d;
    d.lease = [s](const std::string&) -> std::shared_ptr<void> { ++s->leases; return std::shared_ptr<void>(new int(0), [s](void* p) { --s->leases; delete static_cast<int*>(p); }); };
    d.transport = [s](const std::string&) -> std::unique_ptr<msu2::Transport> { return std::make_unique<Fake>(s); };
    d.source = [](const msu2app::Settings&, unsigned id) { return msu2::make_pattern("moving", id); };
    return d;
}
msu2app::Settings settings() { msu2app::Settings s; s.port = kPort; s.timeout_ms = 50; s.hold_open_ms = 0; return s; }
std::string slurp(const std::string& path) { std::ifstream in(path); return std::string(std::istreambuf_iterator<char>(in), {}); }
unsigned count(const std::string& text, const std::string& needle) {
    unsigned n = 0; for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n; return n;
}
bool logs_have(const msu2app::Snapshot& x, const std::string& needle) {
    for (const auto& line : x.logs) if (line.find(needle) != std::string::npos) return true; return false;
}
std::vector<std::string> parse(const std::string& line) {
    std::vector<std::string> f; std::string cur; bool q = false;
    for (std::size_t i = 0; i < line.size(); ++i) { char c = line[i];
        if (c == '"') { if (q && i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; } else q = !q; }
        else if (c == ',' && !q) { f.push_back(cur); cur.clear(); } else cur += c; }
    f.push_back(cur); return f;
}
// Returns the log directory. Exercises one closing-time and one busy-time rejection.
std::string exercise(const std::string& root, bool diagnostics) {
    if (diagnostics) setenv("MSU2_DIAGNOSTICS", "1", 1); else unsetenv("MSU2_DIAGNOSTICS");
    auto s = std::make_shared<Shared>(); std::string dir;
    {
        msu2app::Controller app(root, dependencies(s)); ReleaseGates guard{s}; dir = app.snapshot().log_directory;
        app.connect(settings()); wait_until([&] { auto x = app.snapshot(); return x.connected && !x.busy; }, "connect");
        // Closing-time rejection: the transport destructor is gated inside close().
        s->destroy.arm(); app.disconnect(); s->destroy.await();
        app.connect(settings());
        auto during = app.snapshot();
        check(during.message == kReject, "rejection message must stay unchanged");
        check(s->opens == 1 && s->leases == 1, "rejected connect must not open or lease");
        s->destroy.release();
        wait_until([&] { auto x = app.snapshot(); return !x.connected && !x.busy && s->leases == 0 && x.state == "disconnected"; }, "close");
        auto closed = app.snapshot();
        check(closed.message != kReject, "fixture expects the disconnect completion to replace the message");
        check(logs_have(closed, "拒絕新操作：connect（disconnecting）"), "closing-time rejection must remain in logs");
        // Busy-time rejection: a second request during a gated handshake.
        s->handshake.arm(); app.connect(settings()); s->handshake.await();
        app.send_image(settings());
        check(app.snapshot().message == kReject && s->opens == 2, "busy-time send must be rejected without a new open");
        s->handshake.release();
        wait_until([&] { auto x = app.snapshot(); return x.connected && !x.busy; }, "gated connect completes");
        check(logs_have(app.snapshot(), "拒絕新操作：send（busy）"), "busy-time rejection must remain in logs");
        app.disconnect(); wait_until([&] { auto x = app.snapshot(); return !x.connected && !x.busy && s->leases == 0; }, "final close");
        check(s->opens == 2, "only the two accepted connects may open transports");
    }
    const auto events = slurp(dir + "/events.log");
    check(count(events, "拒絕新操作：connect（disconnecting）") == 1, "events.log must hold the closing-time rejection once");
    check(count(events, "拒絕新操作：send（busy）") == 1, "events.log must hold the busy-time rejection once");
    check(count(events, "拒絕新操作") == 2, "events.log must hold exactly two rejections");
    return dir;
}
void verify_diagnostics(const std::string& dir) {
    std::ifstream in(dir + "/diagnostics.csv"); check(bool(in), "missing diagnostics.csv"); std::string line;
    check(bool(std::getline(in, line)) && line == "t_us,kind,stage,result,frame,patch,v0,v1,v2,v3,v4,v5,rx_len,rx_hex,detail", "CSV header changed");
    unsigned disconnecting = 0, busy = 0, events = 0; std::int64_t end_written = -1;
    while (std::getline(in, line)) {
        auto f = parse(line); check(f.size() == 15, "CSV field count");
        if (f[1] != "log") ++events;
        if (f[1] == "log" && f[2] == "end") end_written = std::stoll(f[6]);
        if (f[1] != "request") continue;
        check(f[2] == "rejected", "unexpected request stage");
        check(f[4] == "-1" && f[5] == "-1", "rejection must not carry frame or patch context");
        for (int i = 6; i <= 11; ++i) check(f[i] == "0", "rejection values must be zero");
        check(f[12] == "0" && f[13].empty(), "rejection must not carry RX bytes");
        check(line.find(kPort) == std::string::npos, "rejection row must not expose the port");
        if (f[3] == "disconnecting" && f[14] == "connect") ++disconnecting;
        else if (f[3] == "busy" && f[14] == "send") ++busy;
        else throw std::runtime_error("unexpected rejection row: " + line);
    }
    check(disconnecting == 1 && busy == 1, "expected one disconnecting/connect and one busy/send row");
    check(end_written == std::int64_t(events), "log end count must match event rows");
}
}  // namespace
int main(int argc, char** argv) {
    try {
        const std::string root = argc > 1 ? argv[1] : "/tmp/msu2-controller-reject-" + std::to_string(getpid());
        const char* old = std::getenv("MSU2_DIAGNOSTICS"); const bool had = old; const std::string saved = old ? old : "";
        const auto off = exercise(root, false);
        check(!std::filesystem::exists(off + "/diagnostics.csv"), "diagnostics must stay off by default");
        const auto on = exercise(root, true);
        verify_diagnostics(on);
        if (had) setenv("MSU2_DIAGNOSTICS", saved.c_str(), 1); else unsetenv("MSU2_DIAGNOSTICS");
        std::cout << "Controller rejection offline tests passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "Controller rejection test failed: " << e.what() << '\n'; return 1; }
}
