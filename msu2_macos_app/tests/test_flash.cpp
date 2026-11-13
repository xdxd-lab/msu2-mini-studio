#include "msu2app/flash.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void check(bool condition, const char* description) {
    if (!condition) throw std::runtime_error(description);
}
template<class Fn> void throws(Fn&& fn, const char* contains) {
    try { fn(); }
    catch (const std::exception& e) {
        if (std::string(e.what()).find(contains) != std::string::npos) return;
        throw std::runtime_error(std::string("unexpected exception: ") + e.what());
    }
    throw std::runtime_error(std::string("expected exception containing: ") + contains);
}

struct FakeFlash final : msu2::Transport {
    std::vector<std::uint8_t> memory = std::vector<std::uint8_t>(msu2app::flash_total_bytes, 0xAA);
    std::array<std::uint8_t, 256> staging{};
    std::deque<std::uint8_t> incoming;
    std::vector<std::vector<std::uint8_t>> writes;
    bool suppress_erase = false;
    bool suppress_commit = false;
    bool corrupt_reads_after_commit = false;
    bool wrong_read_address = false;
    bool wrong_erase_reply = false;
    bool disconnect_commit = false;
    std::size_t fragment = 2;
    unsigned commits = 0;

    void discard_input() override { incoming.clear(); }
    std::size_t read(std::uint8_t* out, std::size_t size, msu2::Milliseconds) override {
        const auto count = std::min({size, fragment, incoming.size()});
        for (std::size_t i = 0; i < count; ++i) { out[i] = incoming.front(); incoming.pop_front(); }
        return count;
    }
    void reply(const std::uint8_t* bytes) { incoming.insert(incoming.end(), bytes, bytes + 6); }
    void write_all(const std::uint8_t* bytes, std::size_t size, msu2::Milliseconds) override {
        writes.emplace_back(bytes, bytes + size);
        check(size % 6 == 0, "fake received incomplete command");
        for (std::size_t i = 0; i < size; i += 6) {
            const auto* c = bytes + i;
            if (c[0] == 4) {
                check(c[1] < 64, "staging index out of range");
                std::copy_n(c + 2, 4, staging.begin() + c[1] * 4);
            } else if (c[0] == 3 && c[1] == 2) {
                const auto page = (unsigned(c[2]) << 8) | c[3];
                const auto count = (unsigned(c[4]) << 8) | c[5];
                std::fill(memory.begin() + page * 256, memory.begin() + (page + count) * 256, 0xFF);
                if (!suppress_erase) {
                    auto response = msu2::Command{3, 2, c[2], c[3], c[4], c[5]};
                    if (wrong_erase_reply) response[1] = 99;
                    reply(response.data());
                }
            } else if (c[0] == 3 && c[1] == 3) {
                if (disconnect_commit) throw std::runtime_error("simulated USB disconnect");
                const auto page = (unsigned(c[2]) << 16) | (unsigned(c[3]) << 8) | c[4];
                check(c[5] == 1, "commit must be one page");
                std::copy(staging.begin(), staging.end(), memory.begin() + page * 256);
                ++commits;
                if (!suppress_commit) reply(c);
            } else if (c[0] == 3 && c[1] == 0) {
                const auto address = (unsigned(c[2]) << 16) | (unsigned(c[3]) << 8) | c[4];
                msu2::Command response{3, 0, c[2], c[3], c[4], memory.at(address)};
                if (wrong_read_address) response[4] ^= 1;
                if (corrupt_reads_after_commit && commits) response[5] ^= 1;
                reply(response.data());
            } else if (c[0] == 2 && c[1] == 3) reply(c);
        }
    }
};

msu2app::FlashPlan plan_at(std::uint32_t page) {
    std::vector<std::uint8_t> bytes(msu2app::flash_image_bytes);
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>((i * 17 + 31) & 255);
    return msu2app::make_flash_plan(std::move(bytes), page);
}
// Success fixtures reply immediately, but host scheduling (especially ASan)
// must not turn a valid fragmented reply into an accidental timeout.
constexpr msu2::Milliseconds success_timeout{1000};
constexpr msu2::Milliseconds expected_timeout{2};
msu2app::FlashOptions success_options(bool verify = true) {
    msu2app::FlashOptions options;
    options.io_timeout = success_timeout;
    options.erase_timeout = success_timeout;
    options.erase_settle = msu2::Milliseconds{0};
    options.verify_readback = verify;
    return options;
}
void test_encoding_and_validation() {
    using namespace msu2app;
    check(flash_erase_command(3826, 100) == msu2::Command{3, 2, 0x0E, 0xF2, 0, 100},
          "background erase wire bytes");
    check(flash_read_command(0x0EF201) == msu2::Command{3, 0, 0x0E, 0xF2, 1, 0},
          "read uses byte address, not page");
    auto plan = plan_at(3996);
    auto packet = flash_page_packet(plan, 99);
    check(packet.size() == 390, "one page is 65 commands");
    check(std::equal(packet.end() - 6, packet.end(), msu2::Command{3, 3, 0, 0x0F, 0xFF, 1}.begin()),
          "last legal page commit bytes");
    check(packet[0] == 4 && packet[1] == 0 && packet[378] == 4 && packet[379] == 63,
          "all 64 staging groups included");
    check(std::equal(packet.begin() + 2, packet.begin() + 6, plan.rgb565.begin() + 99 * 256),
          "RGB565 byte order preserved");
    throws([&] { flash_page_packet(plan, 100); }, "offset");
    throws([&] { make_flash_plan({}, 0); }, "25,600");
    throws([&] { make_flash_plan(std::vector<std::uint8_t>(25601), 0); }, "25,600");
    throws([&] { plan_at(3997); }, "capacity");
    throws([&] { plan_at(std::numeric_limits<std::uint32_t>::max()); }, "capacity");
    throws([&] { flash_erase_command(0, 0); }, "capacity");
    throws([&] { flash_read_command(flash_total_bytes); }, "capacity");
    FakeFlash fake;
    plan.start_page = 4095;
    throws([&] { execute_flash(fake, plan, success_options()); }, "capacity");
    check(fake.writes.empty(), "invalid edited plan must not touch transport");
    throws([&] { read_flash_bytes(fake, flash_total_bytes - 1, 2); }, "capacity");
    check(fake.writes.empty(), "invalid read must not touch transport");
}

void test_roundtrip_and_display() {
    FakeFlash fake;
    const auto plan = plan_at(3826);
    std::vector<msu2app::FlashProgress> events;
    const auto result = msu2app::execute_flash(fake, plan, success_options(),
        [&](const auto& p) { events.push_back(p); });
    check(result.pages_written == 100 && result.bytes_verified == 25600 && !result.cancelled,
          "all pages must have response and every byte must read back");
    check(result.erase_started && result.erase_completed, "erase accounting");
    check(fake.writes.size() == 2 + 1 + 100 + 25600, "read probes, erase, commits, full readback");
    check(fake.writes[2] == std::vector<std::uint8_t>({3, 2, 14, 242, 0, 100}),
          "readback compatibility checked before erase");
    check(std::equal(plan.rgb565.begin(), plan.rgb565.end(), fake.memory.begin() + 3826 * 256),
          "fake external flash contains exact image");
    check(events.front().phase == msu2app::FlashPhase::preflight &&
          events.back().phase == msu2app::FlashPhase::complete, "progress phase sequence");
    check(fake.memory[3826 * 256 - 1] == 0xAA && fake.memory[3926 * 256] == 0xAA,
          "mock page erase does not exceed requested range (not a hardware assertion)");
    msu2app::display_flash_image(fake, 3826, success_timeout);
    check(fake.writes.back() == std::vector<std::uint8_t>({2, 0, 0, 0, 0, 0,
          2, 1, 0, 160, 0, 80, 2, 3, 0, 14, 242, 0}), "Flash display wire bytes");
    const auto last = msu2app::read_flash_bytes(fake, msu2app::flash_total_bytes - 1, 1,
                                               success_timeout);
    check(last == std::vector<std::uint8_t>{0xAA}, "last byte range valid");
}

void test_failure_and_cancellation() {
    const auto plan = plan_at(0);
    {
        FakeFlash fake;
        fake.suppress_erase = true;
        auto options = success_options(false); options.erase_timeout = expected_timeout;
        throws([&] { msu2app::execute_flash(fake, plan, options); }, "Flash erase response timeout");
        check(fake.writes.size() == 1 && fake.commits == 0, "erase timeout must stop without retry");
    }
    {
        FakeFlash fake;
        fake.wrong_erase_reply = true;
        auto options = success_options(false); options.erase_timeout = expected_timeout;
        throws([&] { msu2app::execute_flash(fake, plan, options); }, "response timeout");
        check(fake.commits == 0, "unrelated six-byte response cannot acknowledge erase");
    }
    {
        FakeFlash fake;
        fake.suppress_commit = true;
        auto options = success_options(false); options.io_timeout = expected_timeout;
        throws([&] { msu2app::execute_flash(fake, plan, options); }, "page 0 write response timeout");
        check(fake.commits == 1 && fake.writes.size() == 2, "uncertain page not automatically retried");
    }
    {
        FakeFlash fake;
        fake.disconnect_commit = true;
        throws([&] { msu2app::execute_flash(fake, plan, success_options(false)); }, "USB disconnect");
        check(fake.writes.size() == 2, "transport exceptions propagate without retries");
    }
    {
        FakeFlash fake;
        fake.wrong_read_address = true;
        auto options = success_options(); options.io_timeout = expected_timeout;
        throws([&] { msu2app::execute_flash(fake, plan, options); }, "Flash read");
        check(fake.writes.size() == 1 && fake.memory[0] == 0xAA,
              "incompatible read response must fail before destructive erase");
    }
    {
        FakeFlash fake;
        fake.corrupt_reads_after_commit = true;
        throws([&] { msu2app::execute_flash(fake, plan, success_options()); }, "readback mismatch");
        check(fake.commits == 100, "write replies cannot hide readback mismatch");
    }
    {
        FakeFlash fake;
        const auto result = msu2app::execute_flash(fake, plan, success_options(), {}, [] { return true; });
        check(result.cancelled && !result.erase_started && fake.writes.empty(), "cancel before erase is harmless");
    }
    {
        FakeFlash fake;
        bool stop = false;
        const auto result = msu2app::execute_flash(fake, plan, success_options(false),
            [&](const msu2app::FlashProgress& p) {
                if (p.phase == msu2app::FlashPhase::writing && p.completed == 1) stop = true;
            }, [&] { return stop; });
        check(result.cancelled && result.pages_written == 1 && result.erase_completed,
              "cancellation must retain partial work accounting");
        check(fake.commits == 1 && fake.writes.size() == 2, "cancel occurs between committed pages");
    }
    {
        FakeFlash fake;
        auto options = success_options(); options.io_timeout = msu2::Milliseconds{0};
        throws([&] { msu2app::execute_flash(fake, plan, options); }, "positive");
        check(fake.writes.empty(), "invalid timing fails before I/O");
    }
}
} // namespace

int main() {
    try {
        test_encoding_and_validation();
        test_roundtrip_and_display();
        test_failure_and_cancellation();
        std::cout << "Flash offline tests passed (fake transport; no hardware opened)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
