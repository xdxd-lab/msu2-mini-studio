// Bounded, read-only handshake diagnostic apart from the established MSNCN command.
// It never sends LCD data, firmware commands, or traffic to any other port.
#include "msu2/transport.hpp"
#include "msu2/protocol.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

static bool read_pattern(msu2::SerialTransport& port, const std::vector<std::uint8_t>& pattern,
                         std::chrono::milliseconds duration, const char* label) {
    std::array<std::uint8_t, 512> buf{};
    std::vector<std::uint8_t> pending;
    const auto start = Clock::now();
    const auto deadline = start + duration;
    while (Clock::now() < deadline) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now());
        const auto n = port.read(buf.data(), buf.size(), std::min(left, 50ms));
        if (!n) continue;
        const double ms = std::chrono::duration<double, std::milli>(Clock::now()-start).count();
        std::cout << label << " RX " << ms << "ms " << n << " bytes:";
        for (std::size_t i=0; i<n; ++i) std::printf(" %02x", unsigned(buf[i]));
        std::cout << '\n';
        pending.insert(pending.end(), buf.begin(), buf.begin()+n);
        if (std::search(pending.begin(), pending.end(), pattern.begin(), pattern.end()) != pending.end())
            return true;
        if (pending.size() > pattern.size()-1)
            pending.erase(pending.begin(), pending.end()-(pattern.size()-1));
    }
    return false;
}

int main(int argc, char** argv) {
    if (argc != 2 || std::string(argv[1]) != "/dev/cu.usbmodem01234567891") {
        std::cerr << "Only the already identified MSU2 port is allowed\n";
        return 2;
    }
    try {
        msu2::SerialTransport port(argv[1]);
        const std::vector<std::uint8_t> banner{0, 'M','S','N','0','1'};
        const bool saw_banner = read_pattern(port, banner, 1000ms, "passive");
        std::cout << "passive_banner=" << saw_banner << '\n';
        if (!saw_banner) {
            const auto& command = msu2::handshake_command;
            std::cout << "TX established MSNCN\n";
            port.write_all(command.data(), command.size(), 2000ms);
            const bool echo = read_pattern(port, {command.begin(), command.end()}, 2000ms, "active");
            std::cout << "exact_echo=" << echo << '\n';
            return echo ? 0 : 1;
        }
        return 0;
    } catch(const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
