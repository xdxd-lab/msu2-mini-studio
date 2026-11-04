#import "ui_views.hpp"
#include "msu2/image.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
namespace msu2ui {
CGFloat scale=1;
NSColor* hex(unsigned v){return [NSColor colorWithSRGBRed:((v>>16)&255)/255. green:((v>>8)&255)/255. blue:(v&255)/255. alpha:1];}
NSColor* background(){return hex(0x11151C);}NSColor* panel(){return hex(0x1B222C);}NSColor* control(){return hex(0x273241);}
NSColor* border(){return hex(0x46566B);}NSColor* primary(){return hex(0x4B9EFF);}NSColor* text(){return hex(0xF2F5FA);}NSColor* secondary(){return hex(0xB6C2D1);}
}
@implementation MSU2FlippedView
- (BOOL)isFlipped{return YES;}
@end
@implementation MSU2TouchButton
- (NSSize)intrinsicContentSize{
    NSDictionary *attrs=@{NSFontAttributeName:[NSFont systemFontOfSize:18*msu2ui::scale weight:NSFontWeightSemibold]};
    CGFloat width=[self.title sizeWithAttributes:attrs].width+32*msu2ui::scale+(self.symbolName.length?32*msu2ui::scale:0);
    return NSMakeSize(std::ceil(width),60*msu2ui::scale);
}
- (BOOL)acceptsFirstResponder{return self.enabled;}
- (void)setEnabled:(BOOL)value{[super setEnabled:value];self.needsDisplay=YES;}
- (void)setState:(NSControlStateValue)value{[super setState:value];self.needsDisplay=YES;}
- (void)setSelectedStyle:(BOOL)value{_selectedStyle=value;self.needsDisplay=YES;}
- (void)drawRect:(NSRect)dirty{
    (void)dirty;using namespace msu2ui;NSRectClip(self.bounds);
    const BOOL selected=self.selectedStyle||self.state==NSControlStateValueOn;
    const BOOL blue=(self.primaryAction||selected)&&self.enabled;
    NSColor *fill=blue?primary():control();
    if(self.highlighted&&self.enabled)fill=hex(0x354E6C);
    if(!self.enabled)fill=hex(0x202B38);
    auto *path=[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds,1,1) xRadius:12*scale yRadius:12*scale];
    [fill setFill];[path fill];[(selected?primary():border()) setStroke];path.lineWidth=selected?2:1;[path stroke];
    NSColor *fg=!self.enabled?hex(0xA2AFBE):(self.highlighted?text():(blue?hex(0x081A2E):text()));
    NSDictionary *attributes=@{NSFontAttributeName:[NSFont systemFontOfSize:18*scale weight:NSFontWeightSemibold],NSForegroundColorAttributeName:fg};
    auto titleSize=[self.title sizeWithAttributes:attributes];CGFloat iconSize=self.symbolName.length?22*scale:0;
    CGFloat width=titleSize.width+iconSize+(iconSize?10*scale:0),x=(self.bounds.size.width-width)/2;
    if(iconSize){auto *icon=[NSImage imageWithSystemSymbolName:self.symbolName accessibilityDescription:nil];
        icon=[icon imageWithSymbolConfiguration:[NSImageSymbolConfiguration configurationWithHierarchicalColor:fg]];
        [icon drawInRect:NSMakeRect(x,(self.bounds.size.height-iconSize)/2,iconSize,iconSize)];x+=iconSize+10*scale;}
    [self.title drawInRect:NSMakeRect(x,(self.bounds.size.height-titleSize.height)/2,titleSize.width,titleSize.height) withAttributes:attributes];
    if(self.window.firstResponder==self){auto *focus=[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds,4,4) xRadius:10*scale yRadius:10*scale];[(blue&&!self.highlighted?hex(0x081A2E):primary()) setStroke];focus.lineWidth=3;[focus stroke];}
}
@end
@implementation MSU2NumberCell
- (NSRect)textFrame:(NSRect)frame{CGFloat h=self.font.pointSize+8;return NSMakeRect(frame.origin.x+8,NSMidY(frame)-h/2,frame.size.width-16,h);}
- (void)drawInteriorWithFrame:(NSRect)frame inView:(NSView*)view{[super drawInteriorWithFrame:[self textFrame:frame] inView:view];}
- (void)selectWithFrame:(NSRect)frame inView:(NSView*)view editor:(NSText*)editor delegate:(id)delegate start:(NSInteger)start length:(NSInteger)length{[super selectWithFrame:[self textFrame:frame] inView:view editor:editor delegate:delegate start:start length:length];}
- (void)editWithFrame:(NSRect)frame inView:(NSView*)view editor:(NSText*)editor delegate:(id)delegate event:(NSEvent*)event{[super editWithFrame:[self textFrame:frame] inView:view editor:editor delegate:delegate event:event];}
@end
@implementation MSU2PopupCell
- (NSSize)cellSize{NSSize s=[super cellSize];s.height=60*msu2ui::scale;return s;}
- (void)drawWithFrame:(NSRect)frame inView:(NSView*)view{
    using namespace msu2ui;auto *path=[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(frame,1,1) xRadius:12*scale yRadius:12*scale];
    [control() setFill];[path fill];[border() setStroke];path.lineWidth=1;[path stroke];
    NSString *value=self.title;NSDictionary *attrs=@{NSFontAttributeName:[NSFont systemFontOfSize:18*scale],NSForegroundColorAttributeName:self.enabled?text():hex(0xA2AFBE)};
    NSRect textRect=NSMakeRect(frame.origin.x+16*scale,frame.origin.y+(frame.size.height-25*scale)/2,frame.size.width-54*scale,27*scale);
    auto *style=[NSMutableParagraphStyle new];style.lineBreakMode=NSLineBreakByTruncatingMiddle;
    NSMutableDictionary *values=[attrs mutableCopy];values[NSParagraphStyleAttributeName]=style;[value drawInRect:textRect withAttributes:values];
    auto *arrow=[NSBezierPath bezierPath];CGFloat x=NSMaxX(frame)-24*scale,y=NSMidY(frame);
    CGFloat direction=view.isFlipped?1:-1;
    [arrow moveToPoint:NSMakePoint(x-6*scale,y-direction*3*scale)];[arrow lineToPoint:NSMakePoint(x,y+direction*3*scale)];[arrow lineToPoint:NSMakePoint(x+6*scale,y-direction*3*scale)];
    [secondary() setStroke];arrow.lineWidth=2;[arrow stroke];
    if(view.window.firstResponder==view){[primary() setStroke];path.lineWidth=3;[path stroke];}
}
@end
@implementation MSU2PreviewView
- (instancetype)initWithFrame:(NSRect)frame{if((self=[super initWithFrame:frame])){_autoContrast=YES;desired=actual=msu2app::PreviewBorder::dual;[self setAccessibilityLabel:@"160 × 80 來源畫面預覽"];self.wantsLayer=YES;}return self;}
- (BOOL)isFlipped{return YES;}
- (void)setAutoContrast:(BOOL)value{_autoContrast=value;stability.reset();actual=msu2app::PreviewBorder::dual;self.needsDisplay=YES;}
- (void)setPreview:(const msu2::Image&)image{
    if(image.rgb.empty()){original={};quantized.clear();bitmap=nil;stability.reset();desired=actual=msu2app::PreviewBorder::dual;self.needsDisplay=YES;return;}
    original=image;msu2::to_rgb565(image,quantized);desired=msu2app::analyze_preview_edges(quantized).choice;
    auto *rep=[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:160 pixelsHigh:80 bitsPerSample:8 samplesPerPixel:3 hasAlpha:NO isPlanar:NO colorSpaceName:NSCalibratedRGBColorSpace bytesPerRow:480 bitsPerPixel:24];
    for(size_t i=0;i<12800;++i){unsigned p=(unsigned(quantized[i*2])<<8)|quantized[i*2+1];auto *out=rep.bitmapData+i*3;out[0]=((p>>11)&31)*255/31;out[1]=((p>>5)&63)*255/63;out[2]=(p&31)*255/31;}
    bitmap=[[NSImage alloc] initWithSize:NSMakeSize(160,80)];[bitmap addRepresentation:rep];[self updateBorder];self.needsDisplay=YES;
}
- (void)updateBorder{if(_autoContrast){actual=stability.update(desired,NSProcessInfo.processInfo.systemUptime*1000);self.needsDisplay=YES;}}
- (NSRect)contentRect{CGFloat margin=8;CGFloat factor=std::max(CGFloat(.01),std::min((self.bounds.size.width-2*margin)/160,(self.bounds.size.height-2*margin)/80));NSRect r=NSMakeRect((self.bounds.size.width-160*factor)/2,(self.bounds.size.height-80*factor)/2,160*factor,80*factor);NSRect aligned=[self backingAlignedRect:r options:NSAlignMinXNearest|NSAlignMinYNearest|NSAlignMaxXNearest|NSAlignMaxYNearest];r.origin=aligned.origin;return r;}
- (void)drawRect:(NSRect)dirty{
    (void)dirty;using namespace msu2ui;[hex(0xECECEC) setFill];NSRectFill(self.bounds);NSRect rect=[self contentRect];
    if(bitmap){[NSGraphicsContext saveGraphicsState];NSGraphicsContext.currentContext.imageInterpolation=NSImageInterpolationNone;
        [bitmap drawInRect:rect fromRect:NSZeroRect operation:NSCompositingOperationCopy fraction:1 respectFlipped:YES hints:nil];[NSGraphicsContext restoreGraphicsState];}
    else{[hex(0xDADADA) setFill];NSRectFill(rect);NSString *placeholder=@"160 × 80";auto *attrs=@{NSFontAttributeName:[NSFont systemFontOfSize:16],NSForegroundColorAttributeName:hex(0x555555)};NSSize sz=[placeholder sizeWithAttributes:attrs];[placeholder drawAtPoint:NSMakePoint(NSMidX(rect)-sz.width/2,NSMidY(rect)-sz.height/2) withAttributes:attrs];}
    [NSGraphicsContext saveGraphicsState];
    // Exclude the complete image rectangle from every decoration, including antialiasing.
    auto *clip=[NSBezierPath bezierPathWithRect:self.bounds];[clip appendBezierPathWithRect:rect];clip.windingRule=NSWindingRuleEvenOdd;[clip addClip];
    CGFloat line=1.5*scale;auto stroke=[&](NSColor *color,CGFloat offset){[color setStroke];auto *p=[NSBezierPath bezierPathWithRect:NSInsetRect(rect,-offset,-offset)];p.lineWidth=line;[p stroke];};
    if(!_autoContrast)stroke(hex(0x777777),line/2);
    else if(actual==msu2app::PreviewBorder::dual){stroke(hex(0x111111),line/2);stroke(NSColor.whiteColor,line*1.5);}
    else stroke(actual==msu2app::PreviewBorder::light?NSColor.whiteColor:hex(0x111111),line/2);
    stroke(hex(0x777777),line*2.5);[NSGraphicsContext restoreGraphicsState];
}
- (NSString*)borderDescription{if(!_autoContrast)return @"固定輪廓";switch(actual){case msu2app::PreviewBorder::dark:return @"深色外框";case msu2app::PreviewBorder::light:return @"淺色外框";case msu2app::PreviewBorder::dual:return @"深淺雙色外框";}return @"";}
- (const std::vector<std::uint8_t>&)sourceBytes{return quantized;}
@end
