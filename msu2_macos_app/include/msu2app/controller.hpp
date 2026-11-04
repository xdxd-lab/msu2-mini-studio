#pragma once
#include "msu2/image.hpp"
#include "msu2/transport.hpp"
#include <functional>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace msu2app {
struct Settings {
    std::string port;
    std::string source="desktop"; // desktop, image, moving, bars, gradient, checker, solid, noise
    std::string image_path;
    std::shared_ptr<const msu2::Image> prepared_image; // freezes confirmed flash pixels
    unsigned fps=30;
    std::string priority="off";
    bool differential=true;
    bool pair=true;
    unsigned hold_open_ms=13;
    unsigned timeout_ms=2000;
    unsigned chunk=60;
    bool auto_reconnect=false;
};
struct Snapshot {
    std::string state="disconnected", message="尚未連線", port, log_directory;
    int firmware_banner=-1;
    bool connected=false, streaming=false, paused=false, busy=false;
    std::uint64_t sent=0, unchanged=0, dropped=0, bytes=0;
    std::uint64_t capture_dropped=0, queue_dropped=0; // capture slots missed / waiting frames replaced
    std::uint64_t handshake_failures=0, ack_timeouts=0, transport_errors=0, reconnects=0;
    // Rates are cumulative over the current active stream segment (reset on resume).
    // host_fps counts completed logical updates (possibly multiple rectangles).
    double capture_fps=0, processed_fps=0, host_fps=0;
    double ack_ms=0, write_ms=0, flash_progress=0;
    double download_progress=0;
    std::size_t download_bytes=0;
    std::uint64_t download_generation=0;
    std::string download_directory;
    msu2::Image download_preview;
    std::string active_layout="off", region="full";
    msu2::Image preview;
    std::vector<std::string> logs;
};
// Offline integration tests supply a fake transport and lease; production uses
// SerialGuard and SerialTransport. The dependency is never selected by UI flags.
struct Dependencies {
    std::function<std::shared_ptr<void>(const std::string&)> lease;
    std::function<std::unique_ptr<msu2::Transport>(const std::string&)> transport;
    std::function<msu2::Image(const Settings&, unsigned)> source;
};
class Controller {
public:
    explicit Controller(std::string log_root="");
    Controller(std::string log_root, Dependencies);
    ~Controller();
    Controller(const Controller&)=delete;
    Controller& operator=(const Controller&)=delete;
    void connect(Settings);
    void disconnect();
    void send_image(Settings);
    void start_stream(Settings);
    void pause(bool);
    void stop();
    // Only call after UI shows the concrete page range and user confirms overwrite.
    void flash_image(Settings, unsigned start_page);
    // Reads 100 pages as one 160x80 RGB565 asset; exports into a new subdirectory.
    void download_image(Settings, unsigned start_page, std::string output_directory);
    Snapshot snapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
