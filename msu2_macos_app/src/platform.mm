#include "msu2/trace.hpp"
#include <atomic>
#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <ImageIO/ImageIO.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <IOKit/IOKitLib.h>
#import <IOKit/serial/IOSerialKeys.h>
#include "msu2app/platform.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <stdexcept>

namespace {
const char *message(NSString *value, const char *fallback) { const char *text = value.UTF8String; return text ? text : fallback; }
std::string string_value(CFTypeRef value) {
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) return {};
    return message((__bridge NSString *)value, "");
}
CFTypeRef property(io_registry_entry_t entry, CFStringRef key) {
    return IORegistryEntrySearchCFProperty(entry, kIOServicePlane, key, kCFAllocatorDefault,
                                           kIORegistryIterateRecursively | kIORegistryIterateParents);
}
std::string string_property(io_registry_entry_t entry, CFStringRef key) {
    auto p = property(entry, key);
    auto result = string_value(p);
    if (p) CFRelease(p);
    return result;
}
unsigned number_property(io_registry_entry_t entry, CFStringRef key) {
    auto p = property(entry, key);
    int result = 0;
    if (p && CFGetTypeID(p) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)p, kCFNumberIntType, &result);
    if (p) CFRelease(p);
    return result > 0 ? unsigned(result) : 0;
}
struct CaptureState {
    std::mutex mutex;
    std::condition_variable changed;
    msu2::Image latest;
    msu2app::SourceMetadata metadata;
    CaptureState(){static std::atomic<std::int64_t> next{0};metadata.epoch=++next;}
    std::string error;
};
struct Completion {
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    std::string error;
    SCShareableContent *content = nil;
};
void wait_completion(const std::shared_ptr<Completion>& state, const char* operation) {
    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->changed.wait_for(lock, std::chrono::seconds(5), [&]{ return state->done; }))
        throw std::runtime_error(std::string(operation) + "逾時；請確認螢幕錄製權限，必要時重新啟動程式。");
    if (!state->error.empty()) throw std::runtime_error(state->error);
}
}

@interface MSU2CaptureDelegate : NSObject <SCStreamOutput, SCStreamDelegate> {
@public
    std::shared_ptr<CaptureState> captureState;
}
@end
@implementation MSU2CaptureDelegate
- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error {
    (void)stream;
    std::lock_guard<std::mutex> lock(captureState->mutex);
    captureState->error = message(error.localizedDescription, "桌面擷取中斷");
    captureState->changed.notify_all();
}
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type {
    (void)stream;
    const auto callback_ns=msu2::trace_now_ns();
    if (type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sampleBuffer)) return;
    NSArray *attachments = (__bridge NSArray *)CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
    if (attachments.count) {
        NSNumber *status = attachments[0][SCStreamFrameInfoStatus];
        if (status && status.integerValue != SCFrameStatusComplete) {
            // An idle frame reuses the latest image; suspended/blank/stopped
            // capture must not silently stream that old image forever.
            if (status.integerValue == SCFrameStatusBlank || status.integerValue == SCFrameStatusSuspended || status.integerValue == SCFrameStatusStopped) {
                std::lock_guard<std::mutex> lock(captureState->mutex);
                captureState->error = "桌面擷取已暫停或顯示器不可用；請恢復顯示器後重新開始。";
                captureState->changed.notify_all();
            }
            return;
        }
    }
    CVPixelBufferRef pixels = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!pixels || CVPixelBufferGetPixelFormatType(pixels) != kCVPixelFormatType_32BGRA) return;
    const size_t w = CVPixelBufferGetWidth(pixels), h = CVPixelBufferGetHeight(pixels);
    if (w != 160 || h != 80) return;
    if (CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return;
    const auto *base = static_cast<const std::uint8_t *>(CVPixelBufferGetBaseAddress(pixels));
    const auto stride = CVPixelBufferGetBytesPerRow(pixels);
    msu2::Image result{160, 80, std::vector<std::uint8_t>(160 * 80 * 3)};
    if (base) {
        for (size_t y = 0; y < h; ++y) for (size_t x = 0; x < w; ++x) {
            const auto *p = base + y * stride + x * 4;
            auto *d = result.rgb.data() + (y * w + x) * 3;
            d[0] = p[2]; d[1] = p[1]; d[2] = p[0];
        }
    }
    CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
    if (!base) return;
    std::lock_guard<std::mutex> lock(captureState->mutex);
    // Publish metadata and image under the same lock; idle callbacks do not advance seq.
    captureState->latest = std::move(result);
    auto& meta=captureState->metadata;
    ++meta.sequence;meta.available=true;meta.callback_ns=callback_ns;
    const auto pts=CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    meta.pts_value=pts.value;meta.pts_timescale=pts.timescale;meta.pts_epoch=pts.epoch;
    meta.pts_flags=pts.flags;meta.pts_numeric=CMTIME_IS_NUMERIC(pts)&&pts.timescale>0;
    captureState->changed.notify_all();
}
@end

namespace msu2app {
std::vector<Device> enumerate_devices() {
    std::vector<Device> devices;
    io_iterator_t iterator = IO_OBJECT_NULL;
    CFMutableDictionaryRef match = IOServiceMatching(kIOSerialBSDServiceValue);
    if (!match) return devices;
    CFDictionarySetValue(match, CFSTR(kIOSerialBSDTypeKey), CFSTR(kIOSerialBSDAllTypes));
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &iterator) != KERN_SUCCESS) return devices;
    std::set<std::string> seen;
    for (io_object_t item; (item = IOIteratorNext(iterator)); IOObjectRelease(item)) {
        auto port = string_property(item, CFSTR(kIOCalloutDeviceKey));
        if (port.empty() || !seen.insert(port).second) continue;
        const auto vendor = number_property(item, CFSTR("idVendor"));
        const auto product = number_property(item, CFSTR("idProduct"));
        auto serial = string_property(item, CFSTR("USB Serial Number"));
        auto name = string_property(item, CFSTR("USB Product Name"));
        if (name.empty()) name = port.substr(port.find_last_of('/') + 1);
        char ids[64]{};
        if (vendor || product) snprintf(ids, sizeof(ids), " · %04x:%04x", vendor, product);
        devices.push_back({port, name + ids + " · 未確認", vendor, product, serial});
    }
    IOObjectRelease(iterator);
    std::sort(devices.begin(), devices.end(), [](const Device& a, const Device& b) { return a.port < b.port; });
    return devices;
}
msu2::Image load_image(const std::string& path) {
    @autoreleasepool {
        NSString *file = [NSString stringWithUTF8String:path.c_str()];
        if (!file) throw std::runtime_error("圖片路徑不是有效的 UTF-8。");
        if ([file.pathExtension.lowercaseString isEqualToString:@"ppm"])
            return msu2::resize_crop(msu2::read_ppm(path), 160, 80);
        CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:file], nullptr);
        if (!source) throw std::runtime_error("無法開啟圖片；請選擇 PNG、JPEG 或 PPM。");
        // Decode an orientation-corrected thumbnail, bounding decoded memory for large photos.
        NSDictionary *options = @{(NSString *)kCGImageSourceCreateThumbnailFromImageAlways:@YES,
                                  (NSString *)kCGImageSourceCreateThumbnailWithTransform:@YES,
                                  (NSString *)kCGImageSourceThumbnailMaxPixelSize:@2048};
        CGImageRef image = CGImageSourceCreateThumbnailAtIndex(source, 0, (__bridge CFDictionaryRef)options);
        CFRelease(source);
        if (!image) throw std::runtime_error("圖片解碼失敗。");
        const size_t w = CGImageGetWidth(image), h = CGImageGetHeight(image);
        if (!w || !h || w > 4096 || h > 4096) { CGImageRelease(image); throw std::runtime_error("圖片尺寸無效。"); }
        std::vector<std::uint8_t> rgba(w * h * 4, 0);
        CGColorSpaceRef colors = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef context = CGBitmapContextCreate(rgba.data(), w, h, 8, w * 4, colors,
                                                     kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        CGColorSpaceRelease(colors);
        if (!context) { CGImageRelease(image); throw std::runtime_error("無法建立圖片緩衝區。"); }
        CGContextSetRGBFillColor(context, 0, 0, 0, 1);
        CGContextFillRect(context, CGRectMake(0, 0, w, h));
        CGContextDrawImage(context, CGRectMake(0, 0, w, h), image);
        CGContextRelease(context);
        CGImageRelease(image);
        msu2::Image result{int(w), int(h), std::vector<std::uint8_t>(w * h * 3)};
        for (size_t i = 0; i < w * h; ++i) for (size_t c = 0; c < 3; ++c) result.rgb[i * 3 + c] = rgba[i * 4 + c];
        return msu2::resize_crop(result, 160, 80);
    }
}
void save_image_png(const std::string& path, const msu2::Image& image) {
    if (image.width <= 0 || image.height <= 0 || image.width > 4096 || image.height > 4096 ||
        image.rgb.size() != std::size_t(image.width) * image.height * 3)
        throw std::invalid_argument("匯出圖片的尺寸或像素資料無效。");
    @autoreleasepool {
        NSString *file = [NSString stringWithUTF8String:path.c_str()];
        if (!file) throw std::runtime_error("匯出路徑不是有效的 UTF-8。");
        CFDataRef pixels = CFDataCreate(kCFAllocatorDefault, image.rgb.data(), image.rgb.size());
        CGDataProviderRef provider = pixels ? CGDataProviderCreateWithCFData(pixels) : nullptr;
        if (pixels) CFRelease(pixels);
        CGColorSpaceRef colors = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGImageRef bitmap = provider && colors ? CGImageCreate(image.width, image.height, 8, 24,
            image.width * 3, colors, kCGImageAlphaNone, provider, nullptr, false, kCGRenderingIntentDefault) : nullptr;
        if (provider) CGDataProviderRelease(provider);
        if (colors) CGColorSpaceRelease(colors);
        if (!bitmap) throw std::runtime_error("無法建立 PNG 匯出緩衝區。");
        NSMutableData *data = [NSMutableData data];
        CGImageDestinationRef destination = CGImageDestinationCreateWithData((__bridge CFMutableDataRef)data, CFSTR("public.png"), 1, nullptr);
        if (!destination) { CGImageRelease(bitmap); throw std::runtime_error("無法建立 PNG 編碼器。"); }
        CGImageDestinationAddImage(destination, bitmap, nullptr);
        const bool encoded = CGImageDestinationFinalize(destination);
        CFRelease(destination); CGImageRelease(bitmap);
        if (!encoded) throw std::runtime_error("PNG 編碼失敗。");
        NSError *error = nil;
        if (!data || ![data writeToFile:file options:NSDataWritingAtomic error:&error])
            throw std::runtime_error(message(error.localizedDescription, "PNG 儲存失敗。"));
    }
}
bool capture_permission() { return CGPreflightScreenCaptureAccess(); }
bool request_capture_permission() { return CGRequestScreenCaptureAccess(); }

struct ScreenCapture::Impl {
    std::shared_ptr<CaptureState> state = std::make_shared<CaptureState>();
    SCStream *stream = nil;
    MSU2CaptureDelegate *delegate = nil;
    dispatch_queue_t queue = dispatch_queue_create("com.msu2mini.capture", DISPATCH_QUEUE_SERIAL);
    ~Impl() { if (stream) [stream stopCaptureWithCompletionHandler:nil]; }
};
ScreenCapture::ScreenCapture() : impl_(std::make_unique<Impl>()) {
    if ([NSThread isMainThread]) throw std::runtime_error("桌面擷取必須由背景工作執行。");
    if (!capture_permission()) throw std::runtime_error("尚未允許螢幕錄製。請按「授權桌面擷取」，在系統設定允許本程式後重試。");
    @autoreleasepool {
        auto content = std::make_shared<Completion>();
        [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent *value, NSError *error) {
            std::lock_guard<std::mutex> lock(content->mutex);
            content->content = value;
            if (error) content->error = message(error.localizedDescription, "無法取得桌面");
            content->done = true;
            content->changed.notify_all();
        }];
        wait_completion(content, "取得桌面");
        SCDisplay *display = nil;
        for (SCDisplay *candidate in content->content.displays)
            if (candidate.displayID == CGMainDisplayID()) { display = candidate; break; }
        if (!display) display = content->content.displays.firstObject;
        if (!display) throw std::runtime_error("找不到可擷取的顯示器。");
        SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
        SCStreamConfiguration *config = [SCStreamConfiguration new];
        config.width = 160; config.height = 80;
        config.pixelFormat = kCVPixelFormatType_32BGRA;
        config.minimumFrameInterval = CMTimeMake(1, 60);
        config.queueDepth = 3;
        config.showsCursor = YES;
        config.capturesAudio = NO;
        const double w = display.frame.size.width, h = display.frame.size.height;
        const double cw = std::min(w, h * 2), ch = cw / 2;
        config.sourceRect = CGRectMake((w - cw) / 2, (h - ch) / 2, cw, ch);
        config.scalesToFit = YES;
        impl_->delegate = [MSU2CaptureDelegate new];
        impl_->delegate->captureState = impl_->state;
        impl_->stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:impl_->delegate];
        NSError *outputError = nil;
        if (![impl_->stream addStreamOutput:impl_->delegate type:SCStreamOutputTypeScreen sampleHandlerQueue:impl_->queue error:&outputError])
            throw std::runtime_error(message(outputError.localizedDescription, "無法設定桌面擷取輸出"));
        auto started = std::make_shared<Completion>();
        [impl_->stream startCaptureWithCompletionHandler:^(NSError *error) {
            std::lock_guard<std::mutex> lock(started->mutex);
            if (error) started->error = message(error.localizedDescription, "無法開始桌面擷取");
            started->done = true;
            started->changed.notify_all();
        }];
        wait_completion(started, "開始桌面擷取");
    }
}
ScreenCapture::~ScreenCapture() = default;
msu2::Image ScreenCapture::grab(SourceMetadata* metadata) {
    std::unique_lock<std::mutex> lock(impl_->state->mutex);
    if (!impl_->state->changed.wait_for(lock, std::chrono::seconds(3), [&] {
        return !impl_->state->latest.rgb.empty() || !impl_->state->error.empty();
    })) throw std::runtime_error("尚未收到桌面影格；請確認螢幕錄製權限與顯示器狀態。");
    if (!impl_->state->error.empty()) throw std::runtime_error(impl_->state->error);
    if(metadata)*metadata=impl_->state->metadata;
    return impl_->state->latest;
}
}
