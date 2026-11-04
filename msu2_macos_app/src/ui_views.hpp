#pragma once
#import <Cocoa/Cocoa.h>
#include "msu2/image.hpp"
#include "msu2app/preview.hpp"
namespace msu2ui {
extern CGFloat scale;
NSColor* background(); NSColor* panel(); NSColor* control(); NSColor* border(); NSColor* primary(); NSColor* text(); NSColor* secondary();
NSColor* hex(unsigned value);
}
@interface MSU2FlippedView : NSView
@end
@interface MSU2TouchButton : NSButton
@property(nonatomic) BOOL primaryAction;
@property(nonatomic) BOOL selectedStyle;
@property(nonatomic,copy) NSString *symbolName;
@end
@interface MSU2NumberCell : NSTextFieldCell
@end
@interface MSU2PopupCell : NSPopUpButtonCell
@end
@interface MSU2PreviewView : NSView {
    msu2::Image original;
    std::vector<std::uint8_t> quantized;
    msu2app::BorderStability stability;
    msu2app::PreviewBorder desired, actual;
    NSImage *bitmap;
}
@property(nonatomic) BOOL autoContrast;
- (void)setPreview:(const msu2::Image&)image;
- (void)updateBorder;
- (NSRect)contentRect;
- (NSString*)borderDescription;
- (const std::vector<std::uint8_t>&)sourceBytes;
@end
