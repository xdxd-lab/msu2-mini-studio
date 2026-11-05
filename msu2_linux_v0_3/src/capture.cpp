#include "msu2/capture.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace msu2 {

#ifdef _WIN32
namespace {
constexpr int output_width = 160;
constexpr int output_height = 80;

[[noreturn]] void capture_error(const char* operation) {
    // Several GDI functions do not promise a meaningful GetLastError value.
    throw std::runtime_error(std::string("Windows desktop capture: ") + operation + " failed");
}

void try_process_dpi_awareness() {
    // Best effort, once per process. This changes process-wide DPI behavior and
    // must happen before this prototype creates a window or reads screen metrics.
    // If the embedding application already chose a DPI mode, leave it in place.
    static const bool attempted = [] {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (!user32) return true;

        // Resolve dynamically so older Windows SDKs and Windows versions can
        // still use the legacy fallback without a Shcore dependency.
        using SetContext = BOOL(WINAPI*)(HANDLE);
        auto set_context = reinterpret_cast<SetContext>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (set_context) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 has the value -4.
            const auto context = reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-4));
            if (set_context(context)) return true;
            if (GetLastError() == ERROR_ACCESS_DENIED) return true;
        }

        using SetLegacy = BOOL(WINAPI*)();
        auto set_legacy = reinterpret_cast<SetLegacy>(
            GetProcAddress(user32, "SetProcessDPIAware"));
        if (set_legacy) set_legacy();
        return true;
    }();
    (void)attempted;
}

struct ScreenDC {
    HDC handle = GetDC(nullptr);
    ScreenDC() {
        if (!handle) capture_error("GetDC");
    }
    ~ScreenDC() { ReleaseDC(nullptr, handle); }
    ScreenDC(const ScreenDC&) = delete;
    ScreenDC& operator=(const ScreenDC&) = delete;
};

struct MemoryDC {
    HDC handle;
    explicit MemoryDC(HDC compatible) : handle(CreateCompatibleDC(compatible)) {
        if (!handle) capture_error("CreateCompatibleDC");
    }
    ~MemoryDC() { DeleteDC(handle); }
    MemoryDC(const MemoryDC&) = delete;
    MemoryDC& operator=(const MemoryDC&) = delete;
};

struct Dib {
    HBITMAP handle = nullptr;
    void* pixels = nullptr;

    explicit Dib(HDC compatible) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = output_width;
        info.bmiHeader.biHeight = -output_height; // top-down DIB
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        handle = CreateDIBSection(compatible, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!handle || !pixels) {
            // A throwing constructor does not run this object's destructor.
            if (handle) DeleteObject(handle);
            handle = nullptr;
            capture_error("CreateDIBSection");
        }
    }
    ~Dib() { DeleteObject(handle); }
    Dib(const Dib&) = delete;
    Dib& operator=(const Dib&) = delete;
};

struct SelectedBitmap {
    HDC dc;
    HGDIOBJ previous;
    SelectedBitmap(HDC target, HBITMAP bitmap)
        : dc(target), previous(SelectObject(target, bitmap)) {
        if (!previous || previous == HGDI_ERROR) capture_error("SelectObject");
    }
    ~SelectedBitmap() { SelectObject(dc, previous); }
    SelectedBitmap(const SelectedBitmap&) = delete;
    SelectedBitmap& operator=(const SelectedBitmap&) = delete;
};
} // namespace

struct DesktopCapture::Impl {
    // Member destruction restores the old bitmap before deleting the DIB/DC.
    MemoryDC memory;
    Dib dib;
    SelectedBitmap selection;

    explicit Impl(HDC screen)
        : memory(screen), dib(screen), selection(memory.handle, dib.handle) {
        if (!SetStretchBltMode(memory.handle, HALFTONE)) capture_error("SetStretchBltMode");
        // Required after selecting HALFTONE to avoid brush misalignment.
        if (!SetBrushOrgEx(memory.handle, 0, 0, nullptr)) capture_error("SetBrushOrgEx");
    }

    Image grab() {
        // Reacquire the desktop DC and recalculate the primary-screen crop for
        // each frame, so a primary display resolution change is not cached.
        ScreenDC screen;
        const int screen_width = GetSystemMetrics(SM_CXSCREEN);
        const int screen_height = GetSystemMetrics(SM_CYSCREEN);
        if (screen_width < 2 || screen_height < 1) {
            throw std::runtime_error("Windows desktop capture: primary display has invalid dimensions");
        }

        int crop_width;
        int crop_height;
        if (static_cast<std::int64_t>(screen_width) >= 2LL * screen_height) {
            crop_height = screen_height;
            crop_width = 2 * crop_height;
        } else {
            crop_width = screen_width - screen_width % 2;
            crop_height = crop_width / 2;
        }
        const int source_x = (screen_width - crop_width) / 2;
        const int source_y = (screen_height - crop_height) / 2;

        if (!StretchBlt(memory.handle, 0, 0, output_width, output_height,
                        screen.handle, source_x, source_y, crop_width, crop_height,
                        SRCCOPY | CAPTUREBLT)) {
            capture_error("StretchBlt");
        }
        // Synchronize queued GDI work before directly reading the DIB pixels.
        if (!GdiFlush()) capture_error("GdiFlush");

        Image image;
        image.width = output_width;
        image.height = output_height;
        image.rgb.resize(static_cast<std::size_t>(output_width) * output_height * 3);
        const auto* source = static_cast<const std::uint8_t*>(dib.pixels);
        auto* destination = image.rgb.data();
        // A 32bpp BI_RGB DIB is B,G,R,unused, with a four-byte-aligned row.
        // Here each row is exactly 160 * 4 bytes; the negative height already
        // establishes top-down order, so no vertical flip is needed.
        for (int pixel = 0; pixel < output_width * output_height; ++pixel) {
            destination[0] = source[2];
            destination[1] = source[1];
            destination[2] = source[0];
            source += 4;
            destination += 3;
        }
        return image;
    }
};

DesktopCapture::DesktopCapture() {
    try_process_dpi_awareness();
    ScreenDC screen;
    impl_ = std::make_unique<Impl>(screen.handle);
}

DesktopCapture::~DesktopCapture() = default;

Image DesktopCapture::grab() { return impl_->grab(); }

#else

struct DesktopCapture::Impl {};

DesktopCapture::DesktopCapture() {
    throw std::runtime_error(
        "Desktop capture is supported only on Windows in this prototype; use synthetic or PPM input");
}

DesktopCapture::~DesktopCapture() = default;

Image DesktopCapture::grab() {
    throw std::runtime_error(
        "Desktop capture is supported only on Windows in this prototype; use synthetic or PPM input");
}

#endif

} // namespace msu2
