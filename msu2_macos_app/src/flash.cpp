#include "msu2app/flash.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>

namespace msu2app {
namespace {
using Clock = std::chrono::steady_clock;

void validate_timeout(msu2::Milliseconds timeout) {
    if (timeout.count() <= 0) throw std::invalid_argument("Flash timeout must be positive");
}

void validate_range(std::uint32_t page, std::uint32_t pages) {
    if (!pages || page >= flash_total_pages || pages > flash_total_pages - page)
        throw std::invalid_argument("Flash page range exceeds 4096-page P25D80 capacity");
}

void validate_byte_range(std::uint32_t address, std::size_t length) {
    if (!length || address >= flash_total_bytes || length > flash_total_bytes - address)
        throw std::invalid_argument("Flash byte range exceeds 1 MiB P25D80 capacity");
}

msu2::Command receive_response(msu2::Transport& transport, const msu2::Command& command,
                                std::size_t match_bytes, msu2::Milliseconds timeout,
                                const std::string& operation) {
    const auto deadline = Clock::now() + timeout;
    std::vector<std::uint8_t> pending;
    std::array<std::uint8_t, 128> buffer{};
    while (Clock::now() < deadline) {
        auto remaining = std::max(msu2::Milliseconds{1},
            std::chrono::duration_cast<msu2::Milliseconds>(deadline - Clock::now()));
        const auto count = transport.read(buffer.data(), buffer.size(),
                                         std::min(remaining, msu2::Milliseconds{50}));
        if (count > buffer.size()) throw std::runtime_error("Flash transport returned invalid byte count");
        pending.insert(pending.end(), buffer.begin(), buffer.begin() + count);
        for (std::size_t offset = 0; offset + 6 <= pending.size(); ++offset) {
            if (std::equal(command.begin(), command.begin() + match_bytes,
                           pending.begin() + offset)) {
                msu2::Command reply{};
                std::copy_n(pending.begin() + offset, reply.size(), reply.begin());
                return reply;
            }
        }
        if (pending.size() > 5) pending.erase(pending.begin(), pending.end() - 5);
    }
    throw std::runtime_error(operation + (match_bytes == 5 ? " response timeout; no data was modified"
        : " response timeout; Flash outcome may be partial or unknown (do not automatically retry)"));
}

void send_and_ack(msu2::Transport& transport, const std::uint8_t* data, std::size_t size,
                  const msu2::Command& command, msu2::Milliseconds write_timeout,
                  msu2::Milliseconds reply_timeout, const std::string& operation) {
    transport.discard_input();
    transport.write_all(data, size, write_timeout);
    // Public applications accept any response. We require the operation header
    // and a complete six-byte response, but this is still NOT readback proof.
    receive_response(transport, command, 2, reply_timeout, operation);
}

std::uint8_t read_byte(msu2::Transport& transport, std::uint32_t address,
                      msu2::Milliseconds timeout) {
    const auto command = flash_read_command(address);
    transport.discard_input();
    transport.write_all(command.data(), command.size(), timeout);
    return receive_response(transport, command, 5, timeout,
                            "Flash read at byte " + std::to_string(address))[5];
}

void report(const FlashProgressCallback& progress, FlashPhase phase,
            std::size_t completed, std::size_t total, std::uint32_t page) {
    if (progress) progress({phase, completed, total, page});
}
} // namespace

const char* flash_phase_name(FlashPhase phase) {
    switch (phase) {
        case FlashPhase::preflight: return "preflight";
        case FlashPhase::erasing: return "erasing";
        case FlashPhase::writing: return "writing";
        case FlashPhase::verifying: return "verifying";
        case FlashPhase::complete: return "complete";
        case FlashPhase::cancelled: return "cancelled";
    }
    return "unknown";
}

FlashPlan make_flash_plan(std::vector<std::uint8_t> rgb565, std::uint32_t start_page) {
    FlashPlan result{start_page, std::move(rgb565)};
    validate_flash_plan(result);
    return result;
}

void validate_flash_plan(const FlashPlan& plan) {
    if (plan.rgb565.size() != flash_image_bytes)
        throw std::invalid_argument("Flash image must be exactly 160x80 RGB565 (25,600 bytes)");
    validate_range(plan.start_page, flash_image_pages);
}

msu2::Command flash_erase_command(std::uint32_t start_page, std::uint32_t pages) {
    validate_range(start_page, pages);
    return {3, 2, static_cast<std::uint8_t>(start_page >> 8),
            static_cast<std::uint8_t>(start_page), static_cast<std::uint8_t>(pages >> 8),
            static_cast<std::uint8_t>(pages)};
}

msu2::Command flash_read_command(std::uint32_t byte_address) {
    validate_byte_range(byte_address, 1);
    return {3, 0, static_cast<std::uint8_t>(byte_address >> 16),
            static_cast<std::uint8_t>(byte_address >> 8),
            static_cast<std::uint8_t>(byte_address), 0};
}

std::vector<std::uint8_t> flash_page_packet(const FlashPlan& plan, std::uint32_t page_offset) {
    validate_flash_plan(plan);
    if (page_offset >= plan.page_count()) throw std::invalid_argument("Flash page offset out of range");
    std::vector<std::uint8_t> packet;
    packet.reserve(390);
    const auto offset = static_cast<std::size_t>(page_offset) * 256;
    for (std::uint8_t group = 0; group < 64; ++group) {
        packet.push_back(4);
        packet.push_back(group);
        const auto first = plan.rgb565.begin() + offset + group * 4;
        packet.insert(packet.end(), first, first + 4);
    }
    const auto page = plan.start_page + page_offset;
    packet.insert(packet.end(), {3, 3, static_cast<std::uint8_t>(page >> 16),
        static_cast<std::uint8_t>(page >> 8), static_cast<std::uint8_t>(page), 1});
    return packet;
}

std::vector<std::uint8_t> read_flash_bytes(msu2::Transport& transport,
                                        std::uint32_t byte_address, std::size_t length,
                                        msu2::Milliseconds timeout, FlashCancelCallback cancel,
                                        FlashReadProgressCallback progress) {
    validate_byte_range(byte_address, length);
    validate_timeout(timeout);
    std::vector<std::uint8_t> bytes;
    bytes.reserve(length);
    if (progress) progress(0, length);
    for (std::size_t i = 0; i < length; ++i) {
        if (cancel && cancel()) throw FlashReadCancelled();
        bytes.push_back(read_byte(transport, byte_address + static_cast<std::uint32_t>(i), timeout));
        if (progress && ((i + 1) % 256 == 0 || i + 1 == length)) progress(i + 1, length);
    }
    return bytes;
}

msu2::Image decode_flash_image(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() != flash_image_bytes)
        throw std::invalid_argument("Flash image must be exactly 160x80 RGB565 (25,600 bytes)");
    msu2::Image image{160, 80, std::vector<std::uint8_t>(160 * 80 * 3)};
    for (std::size_t i = 0; i < bytes.size() / 2; ++i) {
        const unsigned pixel = (unsigned(bytes[i * 2]) << 8) | bytes[i * 2 + 1];
        const unsigned r = pixel >> 11, g = (pixel >> 5) & 63, b = pixel & 31;
        image.rgb[i * 3] = (r << 3) | (r >> 2);
        image.rgb[i * 3 + 1] = (g << 2) | (g >> 4);
        image.rgb[i * 3 + 2] = (b << 3) | (b >> 2);
    }
    return image;
}

FlashResult execute_flash(msu2::Transport& transport, const FlashPlan& plan,
                          FlashOptions options, FlashProgressCallback progress,
                          FlashCancelCallback cancel) {
    validate_flash_plan(plan);
    validate_timeout(options.io_timeout);
    validate_timeout(options.erase_timeout);
    if (options.erase_settle.count() < 0)
        throw std::invalid_argument("Flash erase settle time cannot be negative");
    FlashResult result;
    auto is_cancelled = [&] {
        if (cancel && cancel()) {
            result.cancelled = true;
            report(progress, FlashPhase::cancelled, result.pages_written,
                   plan.page_count(), plan.start_page + result.pages_written);
            return true;
        }
        return false;
    };
    report(progress, FlashPhase::preflight, 0, plan.page_count(), plan.start_page);
    if (is_cancelled()) return result;
    if (options.verify_readback) {
        // Check strict read-response compatibility BEFORE any irreversible erase.
        // These probes establish address-echo framing, not chip identity or backup.
        read_byte(transport, plan.start_page * 256, options.io_timeout);
        read_byte(transport, (plan.end_page() + 1) * 256 - 1, options.io_timeout);
    }
    if (is_cancelled()) return result;

    const auto erase = flash_erase_command(plan.start_page, plan.page_count());
    report(progress, FlashPhase::erasing, 0, plan.page_count(), plan.start_page);
    result.erase_started = true;
    send_and_ack(transport, erase.data(), erase.size(), erase, options.io_timeout,
                 options.erase_timeout, "Flash erase");
    result.erase_completed = true;
    std::this_thread::sleep_for(options.erase_settle);
    if (is_cancelled()) return result;

    for (std::uint32_t i = 0; i < plan.page_count(); ++i) {
        if (is_cancelled()) return result;
        const auto packet = flash_page_packet(plan, i);
        msu2::Command commit{};
        std::copy_n(packet.end() - 6, 6, commit.begin());
        send_and_ack(transport, packet.data(), packet.size(), commit, options.io_timeout,
                     options.io_timeout, "Flash page " + std::to_string(plan.start_page + i) + " write");
        ++result.pages_written;
        report(progress, FlashPhase::writing, result.pages_written,
               plan.page_count(), plan.start_page + i);
    }
    if (is_cancelled()) return result;

    if (options.verify_readback) {
        report(progress, FlashPhase::verifying, 0, plan.rgb565.size(), plan.start_page);
        for (std::size_t i = 0; i < plan.rgb565.size(); ++i) {
            // No half-written pages can result from cancellation here. Keep
            // progress/cancel granularity at one 256-byte page to reduce overhead.
            if (i % 256 == 0 && is_cancelled()) return result;
            const auto address = plan.start_page * 256 + static_cast<std::uint32_t>(i);
            const auto actual = read_byte(transport, address, options.io_timeout);
            if (actual != plan.rgb565[i])
                throw std::runtime_error("Flash readback mismatch at byte " + std::to_string(address)
                    + "; written image is not verified (do not automatically retry)");
            ++result.bytes_verified;
            if ((i + 1) % 256 == 0)
                report(progress, FlashPhase::verifying, result.bytes_verified,
                       plan.rgb565.size(), address / 256);
        }
    }
    report(progress, FlashPhase::complete, result.pages_written, plan.page_count(), plan.end_page());
    return result;
}

void display_flash_image(msu2::Transport& transport, std::uint32_t start_page,
                         msu2::Milliseconds timeout) {
    validate_range(start_page, flash_image_pages);
    validate_timeout(timeout);
    const msu2::Command display{2, 3, 0, static_cast<std::uint8_t>(start_page >> 8),
                                static_cast<std::uint8_t>(start_page), 0};
    std::array<std::uint8_t, 18> packet{2, 0, 0, 0, 0, 0,
                                        2, 1, 0, 160, 0, 80};
    std::copy(display.begin(), display.end(), packet.begin() + 12);
    send_and_ack(transport, packet.data(), packet.size(), display, timeout, timeout,
                 "Flash image display");
}

} // namespace msu2app
