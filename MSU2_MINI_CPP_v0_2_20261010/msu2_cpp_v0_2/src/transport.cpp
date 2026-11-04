#include "msu2/transport.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace msu2 {
namespace {
using Clock = std::chrono::steady_clock;

Clock::time_point deadline_after(Milliseconds timeout) {
    if (timeout.count() < 0)
        throw std::invalid_argument("serial timeout must not be negative");
    const auto now = Clock::now();
    const auto available = std::chrono::duration_cast<Milliseconds>(Clock::time_point::max() - now);
    return timeout >= available ? Clock::time_point::max() : now + timeout;
}

// Round up so a positive remainder is never mistaken for an expired deadline.
long long remaining_ms(Clock::time_point deadline) {
    const auto now = Clock::now();
    return now >= deadline ? 0 : std::chrono::ceil<Milliseconds>(deadline - now).count();
}

void check_buffer(const void* ptr, std::size_t size) {
    if (!ptr && size != 0)
        throw std::invalid_argument("serial buffer is null with nonzero length");
}

[[noreturn]] void write_timeout(std::size_t done, std::size_t total) {
    throw std::runtime_error("serial write deadline exceeded after " + std::to_string(done) +
                             " of " + std::to_string(total) +
                             " bytes; the device may have received a partial command");
}

#ifdef _WIN32
[[noreturn]] void windows_error(const std::string& action, DWORD code = GetLastError()) {
    throw std::runtime_error(action + ": " +
                             std::system_category().message(static_cast<int>(code)) +
                             " (Windows error " + std::to_string(code) + ")");
}

bool is_com_name(const std::string& name) {
    if (name.size() < 4 || (name[0] != 'C' && name[0] != 'c') ||
        (name[1] != 'O' && name[1] != 'o') || (name[2] != 'M' && name[2] != 'm'))
        return false;
    return name[3] != '0' && std::all_of(name.begin() + 3, name.end(),
                                      [](char c) { return c >= '0' && c <= '9'; });
}

struct Event {
    HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Event() {
        if (!handle) windows_error("CreateEvent for serial I/O");
    }
    ~Event() { if (handle) CloseHandle(handle); }
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
};

struct Completion {
    DWORD bytes = 0;
    bool timed_out = false;
};

// CancelIoEx only requests cancellation. Always reap the operation before its
// OVERLAPPED, event or caller buffer leaves scope. A malfunctioning Windows
// device driver can delay cancellation completion beyond our requested deadline.
Completion finish_io(HANDLE handle, OVERLAPPED& ov, Clock::time_point deadline,
                     const char* action) {
    for (;;) {
        DWORD bytes = 0;
        if (GetOverlappedResult(handle, &ov, &bytes, FALSE)) return {bytes, false};
        const DWORD error = GetLastError();
        if (error != ERROR_IO_INCOMPLETE) windows_error(action, error);

        const auto remaining = remaining_ms(deadline);
        if (remaining == 0) {
            const BOOL cancelled = CancelIoEx(handle, &ov);
            const DWORD cancel_error = cancelled ? ERROR_SUCCESS : GetLastError();
            const BOOL completed = GetOverlappedResult(handle, &ov, &bytes, TRUE);
            const DWORD completion_error = completed ? ERROR_SUCCESS : GetLastError();
            if (!cancelled && cancel_error != ERROR_NOT_FOUND)
                windows_error(std::string(action) + " cancellation", cancel_error);
            // Normal completion can race with cancellation. Its transferred
            // bytes remain valid and must not be lost or transmitted twice.
            if (completed) return {bytes, false};
            if (completion_error == ERROR_OPERATION_ABORTED) return {bytes, true};
            windows_error(action, completion_error);
        }
        const DWORD wait_ms = static_cast<DWORD>(std::min<long long>(remaining, MAXDWORD - 1));
        const DWORD wait = WaitForSingleObject(ov.hEvent, wait_ms);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_TIMEOUT) continue;
        const DWORD wait_error = wait == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
        CancelIoEx(handle, &ov);
        GetOverlappedResult(handle, &ov, &bytes, TRUE);
        windows_error(std::string(action) + " wait", wait_error);
    }
}

#else
[[noreturn]] void posix_error(const std::string& action, int code = errno) {
    throw std::runtime_error(action + ": " + std::generic_category().message(code) +
                             " (errno " + std::to_string(code) + ")");
}

speed_t baud_constant(unsigned baud) {
    switch (baud) {
    case 1200: return B1200;
    case 2400: return B2400;
    case 4800: return B4800;
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
#ifdef B57600
    case 57600: return B57600;
#endif
#ifdef B115200
    case 115200: return B115200;
#endif
#ifdef B230400
    case 230400: return B230400;
#endif
    default: throw std::invalid_argument("unsupported serial baud: " + std::to_string(baud));
    }
}

// Return revents (including HUP/ERR) so reads can drain already received bytes
// before reporting disconnection. The caller handles the actual I/O outcome.
short await_fd(int fd, short events, Clock::time_point deadline) {
    for (;;) {
        pollfd descriptor{fd, events, 0};
        const int timeout = static_cast<int>(std::min<long long>(remaining_ms(deadline), INT_MAX));
        const int result = ::poll(&descriptor, 1, timeout);
        if (result > 0) {
            if (descriptor.revents & POLLNVAL)
                throw std::runtime_error("serial poll: invalid or closed file descriptor");
            return descriptor.revents;
        }
        if (result == 0) {
            if (Clock::now() >= deadline) return 0;
            continue; // A timeout longer than INT_MAX ms is handled in chunks.
        }
        if (errno != EINTR) posix_error("serial poll");
        if (Clock::now() >= deadline) return 0;
    }
}
#endif
} // namespace

struct SerialTransport::Impl {
    std::string port;
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    DCB original_dcb{};
    COMMTIMEOUTS original_timeouts{};
    bool have_dcb = false;
    bool have_timeouts = false;
    ~Impl() {
        if (handle != INVALID_HANDLE_VALUE) {
            // Public operations leave no outstanding OVERLAPPED requests.
            if (have_dcb) SetCommState(handle, &original_dcb);
            if (have_timeouts) SetCommTimeouts(handle, &original_timeouts);
            CloseHandle(handle);
        }
    }
#else
    int fd = -1;
    termios original{};
    bool have_termios = false;
    int original_modem = 0;
    bool have_modem = false;
    ~Impl() {
        if (fd >= 0) {
            if (have_modem) ::ioctl(fd, TIOCMSET, &original_modem);
            if (have_termios) ::tcsetattr(fd, TCSANOW, &original);
            // Retrying close on EINTR can close an unrelated descriptor on Linux.
            ::close(fd);
        }
    }
#endif
};

SerialTransport::SerialTransport(const std::string& port, unsigned baud)
    : impl_(std::make_unique<Impl>()) {
    if (port.empty()) throw std::invalid_argument("serial port must not be empty");
    impl_->port = port;
#ifdef _WIN32
    if (baud == 0) throw std::invalid_argument("serial baud must not be zero");
    std::string name = port;
    if (name.compare(0, 4, "\\\\.\\") == 0) name.erase(0, 4);
    if (!is_com_name(name))
        throw std::invalid_argument("Windows serial port must be COM followed by a positive number");
    name.replace(0, 3, "COM");
    const std::string device = "\\\\.\\" + name;
    impl_->handle = CreateFileA(device.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (impl_->handle == INVALID_HANDLE_VALUE) windows_error("open serial port " + port);
    impl_->original_dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(impl_->handle, &impl_->original_dcb)) windows_error("GetCommState " + port);
    impl_->have_dcb = true;
    if (!GetCommTimeouts(impl_->handle, &impl_->original_timeouts))
        windows_error("GetCommTimeouts " + port);
    impl_->have_timeouts = true;
    DCB config = impl_->original_dcb;
    config.BaudRate = baud;
    config.ByteSize = 8;
    config.Parity = NOPARITY;
    config.StopBits = ONESTOPBIT;
    config.fBinary = TRUE;
    config.fParity = FALSE;
    config.fOutxCtsFlow = FALSE;
    config.fOutxDsrFlow = FALSE;
    config.fDtrControl = DTR_CONTROL_ENABLE;
    config.fDsrSensitivity = FALSE;
    config.fTXContinueOnXoff = TRUE;
    config.fOutX = FALSE;
    config.fInX = FALSE;
    config.fErrorChar = FALSE;
    config.fNull = FALSE;
    config.fRtsControl = RTS_CONTROL_ENABLE;
    config.fAbortOnError = FALSE;
    if (!SetCommState(impl_->handle, &config)) windows_error("configure serial 8N1 " + port);
    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    if (!SetCommTimeouts(impl_->handle, &timeouts)) windows_error("SetCommTimeouts " + port);
#else
    const speed_t speed = baud_constant(baud);
    int flags = O_RDWR | O_NOCTTY | O_NONBLOCK;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    impl_->fd = ::open(port.c_str(), flags);
    if (impl_->fd < 0) posix_error("open serial port " + port);
    const int fd_flags = ::fcntl(impl_->fd, F_GETFD);
    if (fd_flags < 0 || ::fcntl(impl_->fd, F_SETFD, fd_flags | FD_CLOEXEC) < 0)
        posix_error("set serial close-on-exec " + port);
    if (::tcgetattr(impl_->fd, &impl_->original) < 0) posix_error("read serial settings " + port);
    impl_->have_termios = true;
    termios config = impl_->original;
    ::cfmakeraw(&config);
    config.c_cflag &= ~(CSIZE | PARENB | CSTOPB);
    config.c_cflag |= CS8 | CLOCAL | CREAD;
#ifdef CRTSCTS
    config.c_cflag &= ~CRTSCTS;
#endif
#ifdef CCTS_OFLOW
    config.c_cflag &= ~CCTS_OFLOW;
#endif
#ifdef CRTS_IFLOW
    config.c_cflag &= ~CRTS_IFLOW;
#endif
#ifdef CDTR_IFLOW
    config.c_cflag &= ~CDTR_IFLOW;
#endif
#ifdef CDSR_OFLOW
    config.c_cflag &= ~CDSR_OFLOW;
#endif
#ifdef CCAR_OFLOW
    config.c_cflag &= ~CCAR_OFLOW;
#endif
    config.c_iflag &= ~(IXON | IXOFF | IXANY);
    config.c_cc[VMIN] = 1;
    config.c_cc[VTIME] = 0;
    if (::cfsetispeed(&config, speed) < 0 || ::cfsetospeed(&config, speed) < 0)
        posix_error("set serial baud " + port);
    if (::tcsetattr(impl_->fd, TCSANOW, &config) < 0)
        posix_error("configure serial raw 8N1 " + port);

    // Assert DTR/RTS like the vendor's serial client, without enabling flow
    // control. PTYs and some USB drivers do not implement these modem ioctls.
    if (::ioctl(impl_->fd, TIOCMGET, &impl_->original_modem) == 0) {
        impl_->have_modem = true;
        int modem = impl_->original_modem | TIOCM_DTR | TIOCM_RTS;
        if (::ioctl(impl_->fd, TIOCMSET, &modem) < 0 && errno != ENOTTY && errno != EINVAL)
            posix_error("set serial DTR/RTS " + port);
    } else if (errno != ENOTTY && errno != EINVAL) {
        posix_error("read serial modem state " + port);
    }
#endif
}

SerialTransport::~SerialTransport() = default;

std::size_t SerialTransport::read(std::uint8_t* dst, std::size_t capacity, Milliseconds timeout) {
    const auto deadline = deadline_after(timeout);
    check_buffer(dst, capacity);
    if (capacity == 0) return 0;
#ifdef _WIN32
    // These documented settings return queued bytes immediately, or wait for
    // the first byte. A zero timeout instead polls without waiting for input.
    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    if (timeout.count() != 0) {
        timeouts.ReadTotalTimeoutMultiplier = MAXDWORD;
        timeouts.ReadTotalTimeoutConstant = static_cast<DWORD>(
            std::max<long long>(1, std::min<long long>(remaining_ms(deadline), MAXDWORD - 1)));
    }
    if (!SetCommTimeouts(impl_->handle, &timeouts)) windows_error("set serial read timeout");
    Event event;
    OVERLAPPED ov{};
    ov.hEvent = event.handle;
    DWORD received = 0;
    const DWORD size = static_cast<DWORD>(std::min<std::size_t>(capacity, MAXDWORD));
    if (ReadFile(impl_->handle, dst, size, &received, &ov)) return received;
    const DWORD error = GetLastError();
    if (error != ERROR_IO_PENDING) windows_error("serial read " + impl_->port, error);
    return finish_io(impl_->handle, ov, deadline, "serial read").bytes;
#else
    for (;;) {
        const short events = await_fd(impl_->fd, POLLIN, deadline);
        if (events == 0) return 0;
        const auto size = std::min<std::size_t>(capacity, static_cast<std::size_t>(SSIZE_MAX));
        const ssize_t received = ::read(impl_->fd, dst, size);
        if (received > 0) return static_cast<std::size_t>(received);
        if (received == 0)
            throw std::runtime_error("serial read: device disconnected (EOF): " + impl_->port);
        const int error = errno;
        if (error != EAGAIN && error != EWOULDBLOCK && error != EINTR)
            posix_error("serial read " + impl_->port, error);
        if (events & (POLLHUP | POLLERR))
            throw std::runtime_error("serial read: device disconnected or I/O error: " + impl_->port);
        if (Clock::now() >= deadline) return 0;
    }
#endif
}

void SerialTransport::write_all(const std::uint8_t* src, std::size_t length, Milliseconds timeout) {
    const auto deadline = deadline_after(timeout);
    check_buffer(src, length);
    std::size_t sent = 0;
    while (sent < length) {
#ifdef _WIN32
        Event event;
        OVERLAPPED ov{};
        ov.hEvent = event.handle;
        DWORD transferred = 0;
        const DWORD size = static_cast<DWORD>(std::min<std::size_t>(length - sent, MAXDWORD));
        Completion result;
        if (WriteFile(impl_->handle, src + sent, size, &transferred, &ov)) {
            result.bytes = transferred;
        } else {
            const DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING) windows_error("serial write " + impl_->port, error);
            result = finish_io(impl_->handle, ov, deadline, "serial write");
        }
        sent += result.bytes;
        if (result.timed_out) write_timeout(sent, length);
        if (result.bytes == 0)
            throw std::runtime_error("serial write made no progress: " + impl_->port);
#else
        const short events = await_fd(impl_->fd, POLLOUT, deadline);
        if (events == 0) write_timeout(sent, length);
        if (events & (POLLHUP | POLLERR))
            throw std::runtime_error("serial write: device disconnected or I/O error: " + impl_->port);
        const auto size = std::min<std::size_t>(length - sent, static_cast<std::size_t>(SSIZE_MAX));
        const ssize_t written = ::write(impl_->fd, src + sent, size);
        if (written > 0) {
            sent += static_cast<std::size_t>(written);
        } else if (written == 0) {
            throw std::runtime_error("serial write made no progress: " + impl_->port);
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            posix_error("serial write " + impl_->port);
        }
#endif
        if (sent < length && Clock::now() >= deadline) write_timeout(sent, length);
    }
}

void SerialTransport::discard_input() {
#ifdef _WIN32
    // No TXCLEAR/TXABORT: output already queued must remain untouched.
    if (!PurgeComm(impl_->handle, PURGE_RXCLEAR)) windows_error("discard serial input");
#else
    if (::tcflush(impl_->fd, TCIFLUSH) < 0) posix_error("discard serial input " + impl_->port);
#endif
}

std::vector<std::string> list_serial_ports() {
    std::vector<std::string> ports;
#ifdef _WIN32
    std::vector<char> names(16384);
    for (;;) {
        const DWORD count = QueryDosDeviceA(nullptr, names.data(), static_cast<DWORD>(names.size()));
        if (count != 0) {
            std::size_t pos = 0;
            while (pos < count && names[pos] != '\0') {
                std::string name(names.data() + pos);
                if (is_com_name(name)) ports.push_back(name);
                pos += name.size() + 1;
            }
            break;
        }
        const DWORD error = GetLastError();
        if (error != ERROR_INSUFFICIENT_BUFFER || names.size() >= 1048576)
            windows_error("enumerate serial device names", error);
        names.resize(names.size() * 2);
    }
#else
    std::error_code error;
    std::filesystem::directory_iterator it("/dev", error), end;
    if (error) throw std::runtime_error("enumerate /dev: " + error.message());
    while (it != end) {
        const std::string name = it->path().filename().string();
        const auto starts_with = [&name](const char* prefix) { return name.rfind(prefix, 0) == 0; };
        if (starts_with("ttyACM") || starts_with("ttyUSB") || starts_with("ttyS") ||
            starts_with("cu.") || starts_with("tty.usb") || starts_with("rfcomm"))
            ports.push_back(it->path().string());
        it.increment(error);
        if (error) throw std::runtime_error("enumerate /dev: " + error.message());
    }
#endif
    std::sort(ports.begin(), ports.end());
    ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
    return ports;
}
} // namespace msu2
