#pragma once

#include "msu2/protocol.hpp"
#include "msu2/transport.hpp"
#include "msu2/image.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <stdexcept>
#include <vector>

namespace msu2app {

// MSU2 guide v1.10: P25D80, 1 MiB, 4096 pages of 256 bytes.
inline constexpr std::uint32_t flash_total_pages = 4096;
inline constexpr std::uint32_t flash_total_bytes = flash_total_pages * 256;
inline constexpr std::uint32_t flash_image_pages = 100;
inline constexpr std::size_t flash_image_bytes = 160 * 80 * 2;
inline constexpr std::uint32_t flash_background_page = 3826;
inline constexpr std::uint32_t flash_album_page = 3926;

struct FlashPlan {
    std::uint32_t start_page = flash_background_page;
    std::vector<std::uint8_t> rgb565;
    std::uint32_t page_count() const { return flash_image_pages; }
    std::uint32_t end_page() const { return start_page + page_count() - 1; }
};

// RGB565 is already fitted to 160x80, high byte first. No file/header bytes.
// Validates every input before any device operation; never truncates an image.
FlashPlan make_flash_plan(std::vector<std::uint8_t> rgb565, std::uint32_t start_page);
void validate_flash_plan(const FlashPlan& plan);

enum class FlashPhase { preflight, erasing, writing, verifying, complete, cancelled };
const char* flash_phase_name(FlashPhase phase);
struct FlashProgress {
    FlashPhase phase = FlashPhase::preflight;
    std::size_t completed = 0;
    std::size_t total = 0;
    std::uint32_t page = 0;
};
using FlashProgressCallback = std::function<void(const FlashProgress&)>;
using FlashCancelCallback = std::function<bool()>;
using FlashReadProgressCallback = std::function<void(std::size_t, std::size_t)>;
struct FlashReadCancelled : std::runtime_error {
    FlashReadCancelled() : std::runtime_error("Flash read cancelled; no data was modified") {}
};

struct FlashOptions {
    msu2::Milliseconds io_timeout{3000};
    msu2::Milliseconds erase_timeout{10000};
    msu2::Milliseconds erase_settle{200};
    bool verify_readback = true;
};

struct FlashResult {
    std::uint32_t pages_written = 0;
    std::size_t bytes_verified = 0;
    bool erase_started = false;
    bool erase_completed = false; // A command reply was received, not readback proof.
    bool cancelled = false;
};

// These functions do not open, claim, or handshake a device. Caller must own the
// shared serial guard and an already handshaken Transport exclusively on one
// worker thread. Never run concurrently with Session::send. Any exception means
// the outcome may be partial/unknown; dispose of the session, never auto-retry.
// Command replies are not persistence evidence. verify_readback checks all bytes.
// Cancellation waits until erase/page commit finishes; partial work is retained.
FlashResult execute_flash(msu2::Transport& transport, const FlashPlan& plan,
                          FlashOptions options = {},
                          FlashProgressCallback progress = {},
                          FlashCancelCallback cancel = {});

// Readback returns only data whose six-byte reply echoes the requested address.
// Bounds checked before any I/O. Throws on cancellation (no partial return).
std::vector<std::uint8_t> read_flash_bytes(msu2::Transport& transport,
                                         std::uint32_t byte_address,
                                         std::size_t length,
                                         msu2::Milliseconds timeout = msu2::Milliseconds{3000},
                                         FlashCancelCallback cancel = {},
                                         FlashReadProgressCallback progress = {});
// Decode exactly one stored 160x80, high-byte-first RGB565 asset. No I/O.
msu2::Image decode_flash_image(const std::vector<std::uint8_t>& rgb565);
void display_flash_image(msu2::Transport& transport, std::uint32_t start_page,
                         msu2::Milliseconds timeout = msu2::Milliseconds{3000});

// Exposed for offline command preview and fixture verification. No I/O.
msu2::Command flash_erase_command(std::uint32_t start_page, std::uint32_t pages);
msu2::Command flash_read_command(std::uint32_t byte_address);
std::vector<std::uint8_t> flash_page_packet(const FlashPlan& plan, std::uint32_t page_offset);

} // namespace msu2app
