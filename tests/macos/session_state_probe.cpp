// Isolated diagnostic: unchanged protocol, image, Session and SerialTransport.
// Observes RX announcements and tests established handshake recovery on one fd.
#include "msu2/image.hpp"
#include "msu2/session.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
static double ms(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

// Keep five trailing bytes so banners split across read calls are counted once.
struct BannerScanner {
    std::vector<std::uint8_t> tail;
    unsigned count = 0;
    int last_version = -1;
    void feed(const std::uint8_t* data, std::size_t size) {
        tail.insert(tail.end(), data, data + size);
        for (std::size_t i = 0; i + 6 <= tail.size(); ++i) {
            const auto* b = tail.data() + i;
            if (b[0] == 0 && b[1] == 'M' && b[2] == 'S' && b[3] == 'N' &&
                b[4] >= '0' && b[4] <= '9' && b[5] >= '0' && b[5] <= '9') {
                ++count;
                last_version = 10 * (b[4] - '0') + b[5] - '0';
            }
        }
        if (tail.size() > 5) tail.erase(tail.begin(), tail.end() - 5);
    }
};

class ObservedTransport final : public msu2::Transport {
    msu2::Transport& base;
    struct Event {
        double at_ms;
        std::string phase;
        char direction;
        std::size_t size;
        std::vector<std::uint8_t> data;
    };
    Clock::time_point start = Clock::now();
    std::vector<Event> events;
    bool first_send_write = false;
    void record(char direction, const std::uint8_t* data, std::size_t size) {
        Event event{ms(Clock::now() - start), phase, direction, size, {}};
        // Preserve EVERY received byte, but only the first 18 bytes of TX.
        if (data && size) event.data.assign(data, data + (direction == 'R' ? size : std::min(size, std::size_t(18))));
        events.push_back(std::move(event));
    }
public:
    std::string phase = "banner_wait";
    Clock::time_point last_write_done = Clock::now();
    explicit ObservedTransport(msu2::Transport& transport) : base(transport) { events.reserve(20000); }
    void begin_send() { phase = "input_discard"; first_send_write = true; }
    void begin_handshake() { phase = "banner_wait"; first_send_write = false; }
    std::size_t read(std::uint8_t* data, std::size_t size, msu2::Milliseconds timeout) override {
        const auto n = base.read(data, size, timeout);
        record('R', data, n);
        return n;
    }
    void write_all(const std::uint8_t* data, std::size_t size, msu2::Milliseconds timeout) override {
        if (first_send_write) { phase = "window_write"; first_send_write = false; }
        else if (phase == "window_ack_wait" || phase == "payload_write") phase = "payload_write";
        else phase = "handshake_echo_write";
        record('T', data, size);
        base.write_all(data, size, timeout);
        last_write_done = Clock::now();
        record('W', data, size);
        phase = phase == "window_write" ? "window_ack_wait" :
                phase == "handshake_echo_write" ? "handshake_echo_wait" : phase;
    }
    void discard_input() override { base.discard_input(); record('F', nullptr, 0); }
    void dump() const {
        for (const auto& event : events) {
            std::cerr << std::fixed << std::setprecision(6) << "OBS " << event.at_ms << "ms "
                      << event.phase << ' ' << event.direction << " size=" << event.size << " data=";
            for (auto byte : event.data) std::cerr << std::hex << std::setw(2) << std::setfill('0') << unsigned(byte);
            std::cerr << std::dec << std::setfill(' ') << '\n';
        }
    }
};

struct MonitorResult {
    double duration_ms = 0;
    std::size_t rx_bytes = 0;
    unsigned banners = 0;
    double first_banner_ms = -1; // Host RX observation relative to last accepted TX.
};

static MonitorResult monitor(ObservedTransport& transport, int duration_ms, bool stop_after_banner) {
    MonitorResult result;
    BannerScanner scanner;
    const auto start = Clock::now();
    auto deadline = start + msu2::Milliseconds(duration_ms);
    transport.phase = "idle_rx_monitor";
    std::array<std::uint8_t, 512> buffer{};
    while (Clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<msu2::Milliseconds>(deadline - Clock::now());
        const auto n = transport.read(buffer.data(), buffer.size(), std::max(msu2::Milliseconds(1), std::min(remaining, msu2::Milliseconds(50))));
        if (n > buffer.size()) throw std::runtime_error("invalid monitor read size");
        result.rx_bytes += n;
        const auto previous = scanner.count;
        scanner.feed(buffer.data(), n);
        if (!previous && scanner.count) {
            result.first_banner_ms = ms(Clock::now() - transport.last_write_done);
            if (stop_after_banner) deadline = std::min(deadline, Clock::now() + msu2::Milliseconds(350));
        }
    }
    result.banners = scanner.count;
    result.duration_ms = ms(Clock::now() - start);
    return result;
}

static std::string quoted(std::string value) {
    std::string result = "\"";
    for (char c : value) { if (c == '"') result += '"'; result += c; }
    return result + '"';
}

class Recording {
    std::ofstream csv;
    unsigned index = 0;
public:
    bool protocol_failure = false;
    explicit Recording(const char* path) : csv(path) {
        if (!csv) throw std::runtime_error("cannot create CSV");
        csv << "index,operation,cycle,chunk,requested_ms,actual_ms,status,window_ack_ms,payload_write_ms,host_ms,wire_bytes,write_calls,rx_bytes,banners,first_banner_ms,failure_phase,error\n";
        csv << std::fixed << std::setprecision(6);
    }
    void row(const std::string& operation, int cycle, int chunk, int requested, double actual,
             const std::string& status, double host_ms = 0, const msu2::TransferStats* stats = nullptr,
             const MonitorResult* received = nullptr, const std::string& phase = "", const std::string& error = "") {
        if (status == "failed") protocol_failure = true;
        csv << index++ << ',' << operation << ',' << cycle << ',' << chunk << ',' << requested << ',' << actual << ',' << status << ',';
        if (stats) csv << stats->window_ack_ms << ',' << stats->payload_write_ms;
        else csv << ',';
        csv << ',' << host_ms << ',';
        if (stats) csv << stats->bytes << ',' << stats->write_calls;
        else csv << ',';
        csv << ',';
        if (received) csv << received->rx_bytes << ',' << received->banners << ',' << received->first_banner_ms;
        else csv << ",,";
        csv << ',' << phase << ',' << quoted(error) << '\n';
        csv.flush();
    }
};

static msu2::EncodedFrame fixed_frame() {
    std::vector<std::uint8_t> pixels;
    msu2::to_rgb565(msu2::make_pattern("moving", 0), pixels);
    msu2::EncodedFrame frame;
    msu2::encode_frame(pixels, {}, msu2::Encoding::pair, frame);
    if (frame.payload.size() != 32952) throw std::runtime_error("unexpected baseline frame payload");
    return frame;
}

static bool handshake(msu2::Session& session, ObservedTransport& observed, Recording& recording,
                      const std::string& operation, int cycle, int chunk) {
    observed.begin_handshake();
    const auto start = Clock::now();
    try {
        const auto version = session.handshake();
        recording.row(operation, cycle, chunk, 0, 0, "completed", ms(Clock::now() - start));
        std::cout << "HANDSHAKE operation=" << operation << " cycle=" << cycle << " version=" << version << std::endl;
        return true;
    } catch (const std::exception& error) {
        recording.row(operation, cycle, chunk, 0, 0, "failed", ms(Clock::now() - start), nullptr, nullptr, observed.phase, error.what());
        return false;
    }
}

static bool send(msu2::Session& session, ObservedTransport& observed, Recording& recording,
                 const msu2::EncodedFrame& frame, const std::string& operation, int cycle, int chunk) {
    observed.begin_send();
    const auto start = Clock::now();
    try {
        const auto stats = session.send(frame);
        recording.row(operation, cycle, chunk, 0, 0, "completed", ms(Clock::now() - start), &stats);
        return true;
    } catch (const std::exception& error) {
        recording.row(operation, cycle, chunk, 0, 0, "failed", ms(Clock::now() - start), nullptr, nullptr, observed.phase, error.what());
        return false;
    }
}

static bool recover(msu2::Session& session, ObservedTransport& observed, Recording& recording,
                    const msu2::EncodedFrame& frame, int cycle, int chunk) {
    // Reuse the current descriptor. No blind handshake command, reopen or retry loop.
    if (!handshake(session, observed, recording, "recovery_handshake", cycle, chunk)) return false;
    if (!send(session, observed, recording, frame, "recovered_full_1", cycle, chunk)) return false;
    return send(session, observed, recording, frame, "recovered_full_2", cycle, chunk);
}

static bool run_plan(const std::string& scenario, int chunk, msu2::Session& session,
                     ObservedTransport& observed, Recording& recording, const msu2::EncodedFrame& frame) {
    if (!handshake(session, observed, recording, "initial_handshake", 0, chunk)) return false;
    if (!send(session, observed, recording, frame, "initial_full", 0, chunk)) return false;
    if (scenario == "passive_idle") {
        for (int cycle = 1; cycle <= 2; ++cycle) {
            std::cout << "BEGIN passive_idle cycle=" << cycle << " max_ms=35000" << std::endl;
            const auto received = monitor(observed, 35000, true);
            recording.row("idle_rx_monitor", cycle, chunk, 35000, received.duration_ms, "completed", 0, nullptr, &received);
            if (!send(session, observed, recording, frame, "after_idle_full", cycle, chunk) &&
                !recover(session, observed, recording, frame, cycle, chunk)) return false;
        }
    } else if (scenario == "silent_idle") {
        std::cout << "BEGIN silent_idle no_read_no_write_ms=30000" << std::endl;
        const auto start = Clock::now();
        std::this_thread::sleep_for(msu2::Milliseconds(30000));
        recording.row("silent_idle", 1, chunk, 30000, ms(Clock::now() - start), "completed");
        if (!send(session, observed, recording, frame, "after_idle_full", 1, chunk) &&
            !recover(session, observed, recording, frame, 1, chunk)) return false;
    } else if (scenario == "active_control") {
        for (int cycle = 1; cycle <= 6; ++cycle) {
            const auto received = monitor(observed, 5000, false);
            recording.row("active_gap_monitor", cycle, chunk, 5000, received.duration_ms, "completed", 0, nullptr, &received);
            if (!send(session, observed, recording, frame, "active_full", cycle, chunk)) return false;
        }
    } else {
        const int gap = scenario == "chunk3840_gap100" ? 100 : 0;
        const auto start = Clock::now();
        std::this_thread::sleep_for(msu2::Milliseconds(gap));
        recording.row("silent_gap", 1, chunk, gap, ms(Clock::now() - start), "completed");
        if (!send(session, observed, recording, frame, "second_full", 1, chunk)) {
            std::cout << "BEGIN chunk_failure_rx_monitor max_ms=30000" << std::endl;
            const auto received = monitor(observed, 30000, true);
            recording.row("failure_rx_monitor", 1, chunk, 30000, received.duration_ms, "completed", 0, nullptr, &received);
            msu2::SessionOptions options;
            options.write_chunk = 60;
            options.handshake_timeout = options.io_timeout = msu2::Milliseconds(2000);
            msu2::Session recovery_session(observed, options);
            // Chunk=60 recovery is a distinct controlled transport setting on the SAME fd.
            if (!recover(recovery_session, observed, recording, frame, 1, 60)) return false;
        }
    }
    return true;
}

class FakeTransport final : public msu2::Transport {
public:
    std::vector<std::uint8_t> pending{0, 'M', 'S', 'N', '0', '1'};
    std::vector<std::uint8_t> written;
    bool ack = true;
    std::size_t read(std::uint8_t* dst, std::size_t size, msu2::Milliseconds) override {
        const auto n = std::min(size, pending.size());
        std::copy_n(pending.begin(), n, dst);
        pending.erase(pending.begin(), pending.begin() + n);
        return n;
    }
    void write_all(const std::uint8_t* data, std::size_t size, msu2::Milliseconds) override {
        written.insert(written.end(), data, data + size);
        if (size == 6 && std::equal(data, data + size, msu2::handshake_command.begin())) pending.assign(data, data + size);
        if (size == 18 && ack) pending = {2, 3, 7, 0, 0, 0};
    }
    void discard_input() override { pending.clear(); }
};

static int self_test(const char* csv_path = nullptr) {
    BannerScanner scanner;
    const std::array<std::uint8_t, 16> bytes{9, 0, 'M', 'S', 'N', '0', '1', 0, 'M', 'S', 'N', '9', '9', 0, 'M', 'S'};
    scanner.feed(bytes.data(), 4);
    scanner.feed(bytes.data() + 4, 8);
    scanner.feed(bytes.data() + 12, 4);
    scanner.feed(bytes.data(), 0);
    if (scanner.count != 2 || scanner.last_version != 99) throw std::runtime_error("split banner scanner failed");
    const auto frame = fixed_frame();
    FakeTransport fake;
    ObservedTransport observed(fake);
    msu2::SessionOptions options;
    options.handshake_timeout = options.io_timeout = msu2::Milliseconds(1);
    options.write_chunk = 60;
    msu2::Session session(observed, options);
    session.handshake();
    const auto offset = fake.written.size();
    observed.begin_send();
    const auto first = session.send(frame);
    if (first.bytes != 32970 || first.write_calls != 551) throw std::runtime_error("full stats failed");
    fake.ack = false;
    observed.begin_send();
    bool failed = false;
    try { session.send(frame); } catch (const std::runtime_error&) { failed = true; }
    if (!failed || session.connected() || observed.phase != "window_ack_wait") throw std::runtime_error("ACK failure state failed");
    fake.pending = {0, 'M', 'S', 'N', '0', '1'};
    observed.begin_handshake();
    if (session.handshake() != 1 || !session.connected()) throw std::runtime_error("same-transport recovery failed");
    fake.ack = true;
    const auto recovered_offset = fake.written.size();
    observed.begin_send(); session.send(frame);
    if (!std::equal(fake.written.begin() + offset, fake.written.begin() + offset + first.bytes,
                    fake.written.begin() + recovered_offset)) throw std::runtime_error("recovered bytes differ");
    options.write_chunk = 3840;
    fake.pending = {0, 'M', 'S', 'N', '0', '1'};
    msu2::Session large(observed, options);
    observed.begin_handshake(); large.handshake();
    observed.begin_send();
    if (large.send(frame).write_calls != 10) throw std::runtime_error("3840 stats failed");
    if (csv_path) {
        if (std::ifstream(csv_path).good()) throw std::runtime_error("self-test CSV already exists");
        Recording recording(csv_path);
        recording.row("self_test_full", 1, 60, 0, 0, "completed", 36.5, &first);
        MonitorResult received{20050, 24, 4, 19700};
        recording.row("self_test_monitor", 1, 60, 35000, received.duration_ms, "completed", 0, nullptr, &received);
        recording.row("self_test_failed", 1, 60, 0, 0, "failed", 2001, nullptr, nullptr, "window_ack_wait", "quoted, \"error\"");
    }
    std::cout << "PASS split/multiple banner parsing, unchanged full bytes, ACK failure state, strict same-transport handshake recovery, chunk counts; fake transport only\n";
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--self-test") return self_test();
    if (argc == 3 && std::string(argv[1]) == "--self-test-csv") return self_test(argv[2]);
    const std::vector<std::string> scenarios{"passive_idle", "silent_idle", "active_control", "chunk3840_gap0", "chunk3840_gap100"};
    if (argc != 4 || std::string(argv[1]) != "/dev/cu.usbmodem01234567891" ||
        std::find(scenarios.begin(), scenarios.end(), argv[3]) == scenarios.end()) {
        std::cerr << "Usage: session_state_probe KNOWN_PORT NEW_CSV passive_idle|silent_idle|active_control|chunk3840_gap0|chunk3840_gap100\n";
        return 2;
    }
    if (std::ifstream(argv[2]).good()) { std::cerr << "CSV already exists\n"; return 2; }
    try {
        Recording recording(argv[2]);
        const auto frame = fixed_frame();
        const std::string scenario = argv[3];
        const int chunk = scenario.find("chunk3840") == 0 ? 3840 : 60;
        const auto start = Clock::now();
        msu2::SerialTransport serial(argv[1]);
        std::cout << "OPEN_MS=" << ms(Clock::now() - start) << "; scenario=" << scenario << "; chunk=" << chunk << std::endl;
        ObservedTransport observed(serial);
        msu2::SessionOptions options;
        options.write_chunk = chunk;
        options.handshake_timeout = options.io_timeout = msu2::Milliseconds(2000);
        msu2::Session session(observed, options);
        bool complete = false;
        try { complete = run_plan(scenario, chunk, session, observed, recording, frame); }
        catch (...) { observed.dump(); throw; }
        observed.dump();
        std::cout << (complete ? "COMPLETE" : "STOP") << " protocol_probe scenario=" << scenario
                  << "; protocol_failure=" << recording.protocol_failure
                  << "; same_descriptor=1; LCD completion and power state not measured\n";
        return complete && !recording.protocol_failure ? 0 : 1;
    } catch (const std::exception& error) { std::cerr << "error: " << error.what() << '\n'; return 1; }
}
