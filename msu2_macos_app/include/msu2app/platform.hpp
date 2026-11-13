#pragma once
#include "msu2/image.hpp"
#include "msu2app/source_metadata.hpp"
#include <memory>
#include <string>
#include <vector>
namespace msu2app {
// Enumeration never opens a serial device or claims a verified identity.
struct Device { std::string port, label; unsigned vendor=0, product=0; std::string serial; };
std::vector<Device> enumerate_devices();
msu2::Image load_image(const std::string& path); // PNG/JPEG/PPM, crop to 160x80
void save_image_png(const std::string& path, const msu2::Image& image);
bool capture_permission();
bool request_capture_permission(); // explicit user action only
class ScreenCapture {
public:
    ScreenCapture();
    ~ScreenCapture();
    msu2::Image grab(SourceMetadata* metadata=nullptr);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
