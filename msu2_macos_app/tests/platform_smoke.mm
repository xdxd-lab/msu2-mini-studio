#import <Cocoa/Cocoa.h>
#include "msu2app/platform.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void write_png(NSString *path, int width, int height, bool alpha) {
    auto *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:width pixelsHigh:height bitsPerSample:8 samplesPerPixel:alpha ? 4 : 3 hasAlpha:alpha isPlanar:NO colorSpaceName:NSCalibratedRGBColorSpace bytesPerRow:width * (alpha ? 4 : 3) bitsPerPixel:alpha ? 32 : 24];
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        auto *p = rep.bitmapData + (y * width + x) * (alpha ? 4 : 3);
        p[0] = y < height / 2 ? 255 : 0; p[1] = 0; p[2] = y < height / 2 ? 0 : 255;
        if (alpha) p[3] = 0;
    }
    require([[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES], "fixture write failed");
}
}
int main() {
    @autoreleasepool {
        char directory[] = "/tmp/msu2-platform-XXXXXX";
        require(mkdtemp(directory) != nullptr, "temporary directory failed");
        NSString *folder = [NSString stringWithUTF8String:directory];
        try {
            NSString *rgbPath = [folder stringByAppendingPathComponent:@"orientation.png"];
            write_png(rgbPath, 160, 80, false);
            auto rgb = msu2app::load_image(rgbPath.UTF8String);
            require(rgb.width == 160 && rgb.height == 80 && rgb.rgb.size() == 38400, "RGB image dimensions");
            require(rgb.rgb[0] > 245 && rgb.rgb[2] < 5, "top-left should stay red, not vertically flipped");
            const auto bottom = size_t(79 * 160) * 3;
            require(rgb.rgb[bottom] < 5 && rgb.rgb[bottom + 2] > 245, "bottom-left should stay blue");
            NSString *alphaPath = [folder stringByAppendingPathComponent:@"alpha.png"];
            write_png(alphaPath, 320, 320, true);
            auto alpha = msu2app::load_image(alphaPath.UTF8String);
            for (auto value : alpha.rgb) require(value == 0, "transparent image should composite to black");
            const auto ppmPath = std::string(directory) + "/pattern.ppm";
            auto expected = msu2::make_pattern("bars"); msu2::write_ppm(ppmPath, expected);
            auto ppm = msu2app::load_image(ppmPath);
            require(ppm.rgb == expected.rgb, "PPM pixels changed");
            bool failed = false;
            try { (void)msu2app::load_image(std::string(directory) + "/missing.png"); } catch (const std::exception&) { failed = true; }
            require(failed, "missing image must fail");
            [[NSFileManager defaultManager] removeItemAtPath:folder error:nil];
            std::puts("PLATFORM_SMOKE_OK PNG orientation/alpha/crop dimensions, PPM round trip, missing file; no device or screen capture access");
            return 0;
        } catch (const std::exception& e) {
            [[NSFileManager defaultManager] removeItemAtPath:folder error:nil];
            std::fprintf(stderr, "PLATFORM_SMOKE_FAILED %s\n", e.what());
            return 1;
        }
    }
}
