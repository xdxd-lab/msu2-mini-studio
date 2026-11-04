#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace msu2 {
using Milliseconds = std::chrono::milliseconds;
// All operations are synchronous and owned by a single session thread.
// read returns 0 on timeout; disconnect/system errors throw std::runtime_error.
// write_all uses one deadline for the complete buffer, handles partial writes,
// and returns only after the OS has accepted all bytes (not LCD completion).
class Transport {
public:
    virtual ~Transport() = default;
    virtual std::size_t read(std::uint8_t* dst, std::size_t capacity, Milliseconds timeout) = 0;
    virtual void write_all(const std::uint8_t* src, std::size_t length, Milliseconds timeout) = 0;
    virtual void discard_input() = 0;
};

class SerialTransport final : public Transport {
public:
    explicit SerialTransport(const std::string& port, unsigned baud = 19200);
    ~SerialTransport() override;
    SerialTransport(const SerialTransport&) = delete;
    SerialTransport& operator=(const SerialTransport&) = delete;
    std::size_t read(std::uint8_t*, std::size_t, Milliseconds) override;
    void write_all(const std::uint8_t*, std::size_t, Milliseconds) override;
    void discard_input() override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Candidate names only; listing never opens a port or asserts device identity.
std::vector<std::string> list_serial_ports();
}
