#pragma once
#include "msu2/image.hpp"
#include <memory>
namespace msu2 {
// Windows prototype: primary desktop, centered 2:1 crop, 160x80 output.
// Other platforms throw a clear unsupported error; synthetic/PPM input works.
class DesktopCapture {
public:
    DesktopCapture();
    ~DesktopCapture();
    DesktopCapture(const DesktopCapture&) = delete;
    DesktopCapture& operator=(const DesktopCapture&) = delete;
    Image grab();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
