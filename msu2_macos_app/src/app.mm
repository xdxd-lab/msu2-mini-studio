#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import "ui_views.hpp"
#include "msu2app/controller.hpp"
#include "msu2app/platform.hpp"
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>

namespace {
NSString *ns(const std::string& s) { auto *value = [NSString stringWithUTF8String:s.c_str()]; return value ? value : @""; }
std::string utf8(NSString *s) { const char *value = s.UTF8String; return value ? value : ""; }
bool valid_page(NSString *text) {
    auto s = utf8(text);
    return !s.empty() && s.size() <= 4 && std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }) && std::stoul(s) <= 3996;
}
msu2::Image fixture(NSString *name) {
    auto image = msu2::make_pattern("bars");
    if (![name isEqual:@"black"] && ![name isEqual:@"white"] && ![name isEqual:@"horizontal"] && ![name isEqual:@"vertical"] && ![name isEqual:@"edge"]) return image;
    for (int y = 0; y < 80; ++y) for (int x = 0; x < 160; ++x) {
        bool light = [name isEqual:@"white"] || ([name isEqual:@"horizontal"] && y >= 40) || ([name isEqual:@"vertical"] && x >= 80);
        unsigned char value = light ? 255 : 0;
        if ([name isEqual:@"edge"] && (y == 0 || y == 79 || x == 0 || x == 159)) value = (x + y) % 2 ? 255 : 0;
        for (int channel = 0; channel < 3; ++channel) image.rgb[(y * 160 + x) * 3 + channel] = value;
    }
    return image;
}
NSTextField *label(NSString *text, CGFloat size = 13, NSColor *color = nil) {
    auto *v = [NSTextField wrappingLabelWithString:text];
    v.font = [NSFont systemFontOfSize:size];
    v.textColor = color ? color : NSColor.labelColor;
    return v;
}
NSStackView *stack(NSArray<NSView *> *views, NSUserInterfaceLayoutOrientation orientation, CGFloat spacing = 10) {
    auto *v = [NSStackView stackViewWithViews:views];
    v.orientation = orientation;
    v.spacing = spacing;
    v.alignment = orientation == NSUserInterfaceLayoutOrientationVertical ? NSLayoutAttributeLeading : NSLayoutAttributeCenterY;
    v.distribution = NSStackViewDistributionFill;
    return v;
}
NSView *spacer() { auto *v = [NSView new]; [v setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal]; return v; }
NSView *card(NSView *inside) {
    auto *v = [NSView new];
    v.wantsLayer = YES;
    v.layer.backgroundColor = NSColor.controlBackgroundColor.CGColor;
    v.layer.cornerRadius = 12;
    v.layer.borderWidth = 1;
    v.layer.borderColor = [NSColor.separatorColor colorWithAlphaComponent:0.35].CGColor;
    inside.translatesAutoresizingMaskIntoConstraints = NO;
    [v addSubview:inside];
    [NSLayoutConstraint activateConstraints:@[[inside.leadingAnchor constraintEqualToAnchor:v.leadingAnchor constant:18],
        [inside.trailingAnchor constraintEqualToAnchor:v.trailingAnchor constant:-18],
        [inside.topAnchor constraintEqualToAnchor:v.topAnchor constant:16],
        [inside.bottomAnchor constraintEqualToAnchor:v.bottomAnchor constant:-16]]];
    return v;
}
NSButton *button(NSString *title, id target, SEL action) { return [NSButton buttonWithTitle:title target:target action:action]; }
NSPopUpButton *popup(NSArray<NSString *> *titles, NSArray<NSString *> *values, id target) {
    auto *v = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (NSUInteger i = 0; i < titles.count; ++i) { [v addItemWithTitle:titles[i]]; v.lastItem.representedObject = values[i]; }
    v.target = target; v.action = @selector(settingsChanged:);
    return v;
}
void select_value(NSPopUpButton *v, NSString *value) {
    for (NSMenuItem *item in v.itemArray) if ([item.representedObject isEqual:value]) { [v selectItem:item]; break; }
}
NSView *form_row(NSString *title, NSView *control) {
    auto *name = label(title); [name.widthAnchor constraintEqualToConstant:80].active = YES;
    return stack(@[name, control], NSUserInterfaceLayoutOrientationHorizontal);
}
}

@interface MSU2AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate, NSTextFieldDelegate> {
    std::unique_ptr<msu2app::Controller> controller;
    std::shared_ptr<const msu2::Image> confirmedFlashImage;
    std::vector<msu2app::Device> devices;
    msu2::Image localPreview;
    NSWindow *window;
    NSPopUpButton *devicePopup, *sourcePopup, *priorityPopup, *updatePopup, *encodingPopup, *downloadPreset;
    NSTextField *deviceHint, *statusLabel, *imageLabel, *fpsField, *holdField, *timeoutField;
    NSTextField *rateMetric, *framesMetric, *errorsMetric, *latencyMetric, *previewHint, *flashPathLabel, *flashRangeLabel, *pageField;
    NSTextField *downloadPageField, *downloadRangeLabel, *downloadFolderLabel, *downloadStatusLabel;
    NSButton *connectButton, *disconnectButton, *startButton, *pauseButton, *stopButton, *sendButton, *flashButton, *flashCancelButton, *autoReconnectButton;
    NSButton *downloadButton, *downloadCancelButton, *downloadFolderButton, *downloadOpenButton;
    NSTabView *tabs;
    MSU2PreviewView *previewView, *flashPreview, *downloadPreview;
    NSProgressIndicator *flashProgress, *downloadProgress;
    NSTextView *historyText;
    NSTimer *timer;
    NSString *imagePath, *flashPath, *downloadFolder, *snapshotPath, *snapshotTab, *fixtureName, *fixtureState, *matrixPath, *logRoot, *pendingOperation;
    BOOL smokeMode, quitPending, refreshing, previewBusy, sourceImageValid, qaFailed, downloadActive;
    CGFloat requestedWidth, requestedHeight;
    std::string priorLog;
    NSUInteger previewGeneration;
}
@property(nonatomic) BOOL smokeMode;
@property(nonatomic, copy) NSString *snapshotPath;
@property(nonatomic, copy) NSString *snapshotTab;
@property(nonatomic, copy) NSString *fixtureName;
@property(nonatomic, copy) NSString *fixtureState;
@property(nonatomic, copy) NSString *matrixPath;
@property(nonatomic, copy) NSString *logRoot;
@property(nonatomic) CGFloat requestedWidth, requestedHeight;
@property(nonatomic) BOOL qaFailed;
@end

@implementation MSU2AppDelegate
@synthesize smokeMode, snapshotPath, snapshotTab, fixtureName, fixtureState, matrixPath, logRoot, requestedWidth, requestedHeight, qaFailed;
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    try { controller = std::make_unique<msu2app::Controller>(utf8(logRoot)); }
    catch (const std::exception& e) {
        auto *alert = [NSAlert new]; alert.messageText = @"無法啟動 MSU2 Mini Studio"; alert.informativeText = ns(e.what());
        [alert runModal]; [NSApp terminate:nil]; return;
    }
    [self buildMenu];
    [self buildWindow];
    [self restoreSettings];
    if ([snapshotTab isEqualToString:@"flash"]) [tabs selectTabViewItemAtIndex:1];
    if ([snapshotTab isEqualToString:@"download"]) [tabs selectTabViewItemAtIndex:2];
    if ([snapshotTab isEqualToString:@"history"]) [tabs selectTabViewItemAtIndex:3];
    if (smokeMode) {
        [devicePopup addItemWithTitle:@"沒有可用的序列埠"]; devicePopup.lastItem.representedObject = @"";
        [self updateDeviceHint];
        localPreview = fixture(fixtureName ? fixtureName : @"bars");
        [previewView setPreview:localPreview];
        [flashPreview setPreview:localPreview];
        downloadFolder = @"下載資料夾 / MSU2 Mini Studio"; downloadFolderLabel.stringValue = downloadFolder;
        previewHint.stringValue = @"160 × 80 · 離線介面檢查圖樣，尚未傳送至裝置";
    } else {
        [self refreshDevices:nil];
        if ([sourcePopup.selectedItem.representedObject isEqual:@"desktop"])
            previewHint.stringValue = @"按「更新預覽」或「開始串流」擷取桌面。尚未授權時請先按「授權桌面擷取」。";
        else [self updatePreview:nil];
    }
    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    timer = [NSTimer scheduledTimerWithTimeInterval:0.25 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
    if (matrixPath.length) { [self runQACase:0]; return; }
    if (smokeMode || snapshotPath.length) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1500 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
            [self tick:nil];
            if (self->snapshotPath.length) [self writeSnapshot];
            std::puts("UI_SMOKE_OK no serial connection or permission request performed");
            if (self->smokeMode) [NSApp terminate:nil];
        });
    }
}
- (void)buildMenu {
    auto *bar = [NSMenu new]; auto *appItem = [NSMenuItem new]; [bar addItem:appItem];
    auto *appMenu = [NSMenu new];
    [appMenu addItemWithTitle:@"關於 MSU2 Mini Studio" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
    [appMenu addItem:NSMenuItem.separatorItem];
    [appMenu addItemWithTitle:@"結束 MSU2 Mini Studio" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    auto *editItem = [NSMenuItem new]; editItem.title = @"編輯"; [bar addItem:editItem];
    auto *edit = [[NSMenu alloc] initWithTitle:@"編輯"];
    [edit addItemWithTitle:@"剪下" action:@selector(cut:) keyEquivalent:@"x"];
    [edit addItemWithTitle:@"複製" action:@selector(copy:) keyEquivalent:@"c"];
    [edit addItemWithTitle:@"貼上" action:@selector(paste:) keyEquivalent:@"v"];
    [edit addItemWithTitle:@"全選" action:@selector(selectAll:) keyEquivalent:@"a"];
    editItem.submenu = edit; NSApp.mainMenu = bar;
}
- (void)buildWindow {
    window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, requestedWidth > 0 ? requestedWidth : 1120, requestedHeight > 0 ? requestedHeight : 840)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    window.title = @"MSU2 Mini Studio"; window.minSize = NSMakeSize(1080, 810); window.delegate = self;
    window.releasedWhenClosed = NO; [window center];
    window.backgroundColor = [NSColor colorWithCalibratedWhite:0.965 alpha:1];
    window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
    auto *title = label(@"MSU2 Mini Studio", 27); title.font = [NSFont systemFontOfSize:27 weight:NSFontWeightSemibold];
    auto *subtitle = label(@"macOS  ·  圖片燒錄與下載、桌面投射與串流管理", 13, NSColor.secondaryLabelColor);
    auto *heading = stack(@[title, subtitle], NSUserInterfaceLayoutOrientationVertical, 4);
    auto *version = label(@"160 × 80  /  RGB565", 12, NSColor.secondaryLabelColor);
    auto *header = stack(@[heading, spacer(), version], NSUserInterfaceLayoutOrientationHorizontal);
    devicePopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    devicePopup.target = self; devicePopup.action = @selector(deviceChanged:);
    [devicePopup.widthAnchor constraintGreaterThanOrEqualToConstant:420].active = YES;
    connectButton = button(@"連線", self, @selector(connect:));
    disconnectButton = button(@"中斷連線", self, @selector(disconnect:));
    auto *deviceRow = stack(@[label(@"裝置", 14), devicePopup, button(@"重新整理", self, @selector(refreshDevices:)), spacer(), connectButton, disconnectButton], NSUserInterfaceLayoutOrientationHorizontal);
    deviceHint = label(@"正在讀取序列埠清單…", 12, NSColor.secondaryLabelColor);
    deviceHint.maximumNumberOfLines = 2;
    statusLabel = label(@"尚未連線", 13, NSColor.systemBlueColor);
    auto *deviceStack = stack(@[deviceRow, deviceHint, statusLabel], NSUserInterfaceLayoutOrientationVertical, 9);
    [deviceRow.widthAnchor constraintEqualToAnchor:deviceStack.widthAnchor].active = YES;
    auto *deviceCard = card(deviceStack);
    tabs = [[NSTabView alloc] initWithFrame:NSZeroRect];
    tabs.font = [NSFont systemFontOfSize:13];
    for (NSArray *pair in @[@[@"投射與串流", [self streamPane]], @[@"圖片燒錄", [self flashPane]], @[@"圖片下載", [self downloadPane]], @[@"工作紀錄", [self historyPane]]]) {
        auto *item = [[NSTabViewItem alloc] initWithIdentifier:pair[0]]; item.label = pair[0]; item.view = pair[1]; [tabs addTabViewItem:item];
    }
    auto *footer = label(@"候選 USB 裝置需通過協定握手才能確認連線。主機 FPS 與 LCD 實際更新率分開計算；LCD 更新率尚無量測。", 11, NSColor.secondaryLabelColor);
    auto *root = stack(@[header, deviceCard, tabs, footer], NSUserInterfaceLayoutOrientationVertical, 16);
    root.translatesAutoresizingMaskIntoConstraints = NO; [window.contentView addSubview:root];
    [NSLayoutConstraint activateConstraints:@[[root.leadingAnchor constraintEqualToAnchor:window.contentView.leadingAnchor constant:24],
        [root.trailingAnchor constraintEqualToAnchor:window.contentView.trailingAnchor constant:-24],
        [root.topAnchor constraintEqualToAnchor:window.contentView.topAnchor constant:22],
        [root.bottomAnchor constraintEqualToAnchor:window.contentView.bottomAnchor constant:-16],
        [header.widthAnchor constraintEqualToAnchor:root.widthAnchor], [deviceCard.widthAnchor constraintEqualToAnchor:root.widthAnchor],
        [tabs.widthAnchor constraintEqualToAnchor:root.widthAnchor], [tabs.heightAnchor constraintGreaterThanOrEqualToConstant:485]]];
    [tabs setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationVertical];
}
- (NSView *)streamPane {
    sourcePopup = popup(@[@"桌面 · 主顯示器中央裁切", @"圖片 · PNG / JPEG / PPM", @"動態色條", @"靜態色條", @"漸層", @"棋盤格", @"純色", @"雜訊"], @[@"desktop", @"image", @"moving", @"bars", @"gradient", @"checker", @"solid", @"noise"], self);
    [sourcePopup.widthAnchor constraintEqualToConstant:262].active = YES;
    imageLabel = label(@"尚未選擇圖片", 11, NSColor.secondaryLabelColor); imageLabel.maximumNumberOfLines = 2;
    auto *sourceActions = stack(@[button(@"選擇圖片…", self, @selector(chooseImage:)), button(@"更新預覽", self, @selector(updatePreview:))], NSUserInterfaceLayoutOrientationHorizontal);
    fpsField = [NSTextField textFieldWithString:@"30"]; fpsField.delegate = self; [fpsField.widthAnchor constraintEqualToConstant:56].active = YES;
    auto *fpsRow = stack(@[fpsField, label(@"FPS  ·  1–60", 12, NSColor.secondaryLabelColor)], NSUserInterfaceLayoutOrientationHorizontal);
    updatePopup = popup(@[@"差分更新", @"完整畫面"], @[@"diff", @"full"], self);
    encodingPopup = popup(@[@"Pair 壓縮", @"Raw 原始"], @[@"pair", @"raw"], self);
    priorityPopup = popup(@[@"關閉", @"四橫帶優先", @"中央三區", @"左右兩區"], @[@"off", @"bands4", @"center3", @"halves2"], self);
    holdField = [NSTextField textFieldWithString:@"13"]; holdField.delegate = self; [holdField.widthAnchor constraintEqualToConstant:50].active = YES;
    timeoutField = [NSTextField textFieldWithString:@"2000"]; timeoutField.delegate = self; [timeoutField.widthAnchor constraintEqualToConstant:66].active = YES;
    auto *transport = stack(@[label(@"Hold", 12), holdField, label(@"ms", 12), label(@"ACK", 12), timeoutField, label(@"ms", 12)], NSUserInterfaceLayoutOrientationHorizontal, 7);
    autoReconnectButton = [NSButton checkboxWithTitle:@"傳輸中斷後自動重新連線" target:self action:@selector(settingsChanged:)];
    auto *left = stack(@[label(@"輸入與傳送設定", 15), form_row(@"畫面來源", sourcePopup), sourceActions, imageLabel,
        form_row(@"目標速度", fpsRow), form_row(@"畫面更新", updatePopup), form_row(@"編碼方式", encodingPopup),
        form_row(@"優先區域", priorityPopup), transport, autoReconnectButton,
        label(@"變更設定後，停止並重新開始串流即可套用。", 11, NSColor.secondaryLabelColor)], NSUserInterfaceLayoutOrientationVertical, 11);
    previewView = [[MSU2PreviewView alloc] initWithFrame:NSZeroRect];
    [previewView.widthAnchor constraintEqualToConstant:500].active = YES; [previewView.heightAnchor constraintEqualToConstant:264].active = YES;
    previewHint = label(@"160 × 80 · 預覽尚未傳送至裝置", 12, NSColor.secondaryLabelColor); previewHint.maximumNumberOfLines = 3;
    auto *right = stack(@[label(@"畫面預覽", 15), previewView, previewHint,
        button(@"授權桌面擷取", self, @selector(requestPermission:))], NSUserInterfaceLayoutOrientationVertical, 10);
    auto *upper = stack(@[left, spacer(), right], NSUserInterfaceLayoutOrientationHorizontal, 22); upper.alignment = NSLayoutAttributeTop;
    startButton = button(@"開始串流", self, @selector(start:)); startButton.bezelColor = NSColor.systemBlueColor;
    pauseButton = button(@"暫停", self, @selector(pause:)); stopButton = button(@"停止", self, @selector(stop:)); sendButton = button(@"傳送單張", self, @selector(sendOne:));
    auto *actions = stack(@[startButton, pauseButton, stopButton, sendButton, spacer()], NSUserInterfaceLayoutOrientationHorizontal);
    rateMetric = label(@"來源取得  0.0 FPS  /  主機更新  0.0 次/秒\nLCD 實際更新率：未量測", 12);
    framesMetric = label(@"傳送  0  /  未變更  0\n排程丟幀  0", 12);
    errorsMetric = label(@"ACK 逾時  0  /  握手失敗  0\n傳輸錯誤  0  /  重連  0", 12);
    latencyMetric = label(@"ACK  0.0 ms\n寫入  0.0 ms", 12);
    auto *metrics = stack(@[rateMetric, spacer(), framesMetric, spacer(), errorsMetric, spacer(), latencyMetric], NSUserInterfaceLayoutOrientationHorizontal, 12);
    auto *root = stack(@[upper, actions, metrics], NSUserInterfaceLayoutOrientationVertical, 16);
    [upper.widthAnchor constraintEqualToAnchor:root.widthAnchor].active = YES;
    [actions.widthAnchor constraintEqualToAnchor:root.widthAnchor].active = YES;
    [metrics.widthAnchor constraintEqualToAnchor:root.widthAnchor].active = YES;
    return [self paddedPane:root];
}
- (NSView *)flashPane {
    auto *title = label(@"將圖片寫入裝置的外部 Flash", 19); title.font = [NSFont systemFontOfSize:19 weight:NSFontWeightSemibold];
    auto *intro = label(@"圖片會中央裁切為 160 × 80，轉成 RGB565，共 25,600 bytes / 100 頁。這裡只燒錄圖片素材，不提供 MCU 韌體更新。", 13, NSColor.secondaryLabelColor);
    intro.maximumNumberOfLines = 3;
    flashPathLabel = label(@"尚未選擇燒錄圖片", 12, NSColor.secondaryLabelColor); flashPathLabel.maximumNumberOfLines = 2;
    flashPreview = [[MSU2PreviewView alloc] initWithFrame:NSZeroRect];
    [flashPreview.widthAnchor constraintEqualToConstant:424].active = YES; [flashPreview.heightAnchor constraintEqualToConstant:224].active = YES;
    pageField = [NSTextField textFieldWithString:@"3826"]; pageField.delegate = self; [pageField.widthAnchor constraintEqualToConstant:80].active = YES;
    auto *pageRow = stack(@[label(@"起始頁"), pageField, label(@"範圍 0–3996", 12, NSColor.secondaryLabelColor)], NSUserInterfaceLayoutOrientationHorizontal);
    auto *presets = popup(@[@"背景圖：3826", @"相簿圖：3926", @"動畫首張：0", @"自訂頁位址"], @[@"3826", @"3926", @"0", @"custom"], self); presets.action = @selector(flashPreset:);
    flashRangeLabel = label(@"將覆寫第 3826–3925 頁（100 頁）", 13, NSColor.systemOrangeColor);
    flashButton = button(@"檢查範圍並燒錄…", self, @selector(flash:));
    flashCancelButton = button(@"取消燒錄", self, @selector(stop:));
    auto *flashActions = stack(@[flashButton, flashCancelButton], NSUserInterfaceLayoutOrientationHorizontal);
    auto *warning = label(@"沒有保證可用的空白區域。指定範圍原有的背景、相簿或動畫素材可能被覆寫；開始前會再次確認。", 12, NSColor.secondaryLabelColor); warning.maximumNumberOfLines = 4;
    auto *controls = stack(@[button(@"選擇燒錄圖片…", self, @selector(chooseFlashImage:)), flashPathLabel, presets, pageRow, flashRangeLabel, warning, flashActions], NSUserInterfaceLayoutOrientationVertical, 15);
    [controls.widthAnchor constraintLessThanOrEqualToConstant:540].active = YES;
    auto *content = stack(@[controls, spacer(), flashPreview], NSUserInterfaceLayoutOrientationHorizontal, 28); content.alignment = NSLayoutAttributeTop;
    flashProgress = [NSProgressIndicator new]; flashProgress.indeterminate = NO; flashProgress.minValue = 0; flashProgress.maxValue = 1; flashProgress.style = NSProgressIndicatorStyleBar;
    auto *root = stack(@[title, intro, content, flashProgress, label(@"Flash 寫入期間請保持 USB 連線；進度與結果會記錄在工作紀錄。", 12, NSColor.secondaryLabelColor)], NSUserInterfaceLayoutOrientationVertical, 22);
    [content.widthAnchor constraintEqualToAnchor:root.widthAnchor].active = YES;
    [intro.widthAnchor constraintEqualToAnchor:root.widthAnchor].active = YES;
    [flashProgress.widthAnchor constraintEqualToAnchor:root.widthAnchor].active = YES;
    return [self paddedPane:root];
}
- (NSView *)downloadPane {
    auto *title = label(@"從裝置下載已儲存的圖片", 19); title.font = [NSFont systemFontOfSize:19 weight:NSFontWeightSemibold];
    auto *intro = label(@"選擇外部 Flash 的起始頁，連續讀取 100 頁，解讀為一張 160 × 80 RGB565 圖片。完成後保存 PNG、原始 RGB565 與頁面資訊。", 13, NSColor.secondaryLabelColor);
    intro.maximumNumberOfLines = 3;
    downloadPreset = popup(@[@"背景圖：3826", @"相簿圖：3926", @"動畫首張：0", @"自訂頁位址"], @[@"3826", @"3926", @"0", @"custom"], self); downloadPreset.action = @selector(downloadPreset:);
    downloadPageField = [NSTextField textFieldWithString:@"3826"]; downloadPageField.delegate = self;
    [downloadPageField.widthAnchor constraintEqualToConstant:80].active = YES;
    auto *pageRow = stack(@[label(@"起始頁"), downloadPageField, label(@"範圍 0–3996", 12, NSColor.secondaryLabelColor)], NSUserInterfaceLayoutOrientationHorizontal);
    downloadRangeLabel = label(@"", 13);
    downloadFolderButton = button(@"選擇下載資料夾…", self, @selector(chooseDownloadFolder:));
    downloadFolderLabel = label(@"尚未選擇下載資料夾", 12, NSColor.secondaryLabelColor); downloadFolderLabel.maximumNumberOfLines = 3;
    auto *hint = label(@"預設頁位址依已知素材配置提供。下載內容來自指定的儲存頁；實際圖片位置請依你的素材配置選擇。讀取期間會暫停其他裝置操作。", 12, NSColor.secondaryLabelColor); hint.maximumNumberOfLines = 4;
    downloadButton = button(@"讀取並下載圖片", self, @selector(download:)); downloadButton.bezelColor = NSColor.systemBlueColor;
    downloadCancelButton = button(@"取消讀取", self, @selector(stop:));
    downloadOpenButton = button(@"開啟下載結果", self, @selector(openDownload:));
    auto *actions = stack(@[downloadButton, downloadCancelButton], NSUserInterfaceLayoutOrientationHorizontal);
    auto *controls = stack(@[downloadPreset, pageRow, downloadRangeLabel, downloadFolderButton, downloadFolderLabel, hint, actions, downloadOpenButton], NSUserInterfaceLayoutOrientationVertical, 14);
    [controls.widthAnchor constraintLessThanOrEqualToConstant:520].active = YES;
    downloadPreview = [[MSU2PreviewView alloc] initWithFrame:NSZeroRect];
    [downloadPreview.widthAnchor constraintEqualToConstant:424].active = YES; [downloadPreview.heightAnchor constraintEqualToConstant:224].active = YES;
    auto *preview = stack(@[label(@"裝置讀回圖片", 15), downloadPreview, label(@"完整讀取後顯示預覽", 12, NSColor.secondaryLabelColor)], NSUserInterfaceLayoutOrientationVertical, 10);
    auto *content = stack(@[controls, spacer(), preview], NSUserInterfaceLayoutOrientationHorizontal, 24); content.alignment = NSLayoutAttributeTop;
    downloadProgress = [NSProgressIndicator new]; downloadProgress.indeterminate = NO; downloadProgress.minValue = 0; downloadProgress.maxValue = 1; downloadProgress.style = NSProgressIndicatorStyleBar;
    downloadStatusLabel = label(@"請先連線，選擇起始頁與下載資料夾。", 12, NSColor.secondaryLabelColor); downloadStatusLabel.maximumNumberOfLines = 3;
    auto *root = stack(@[title, intro, content, downloadProgress, downloadStatusLabel], NSUserInterfaceLayoutOrientationVertical, 18);
    for (NSView *view in @[intro, content, downloadProgress, downloadStatusLabel]) [view.widthAnchor constraintEqualToAnchor:root.widthAnchor].active = YES;
    return [self paddedPane:root];
}
- (NSView *)historyPane {
    historyText = [[NSTextView alloc] initWithFrame:NSZeroRect]; historyText.editable = NO;
    historyText.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular]; historyText.textColor = NSColor.labelColor;
    historyText.textContainerInset = NSMakeSize(12, 12);
    auto *scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect]; scroll.hasVerticalScroller = YES; scroll.documentView = historyText; scroll.borderType = NSBezelBorder;
    historyText.autoresizingMask = NSViewWidthSizable; historyText.textContainer.widthTracksTextView = YES;
    auto *heading = stack(@[label(@"本次工作紀錄", 17), spacer(), button(@"開啟紀錄資料夾", self, @selector(openLogs:))], NSUserInterfaceLayoutOrientationHorizontal);
    auto *root = stack(@[heading, scroll, label(@"每次工作會保存狀態、傳輸統計與錯誤分類，便於後續驗證。", 12, NSColor.secondaryLabelColor)], NSUserInterfaceLayoutOrientationVertical, 14);
    [heading.widthAnchor constraintEqualToAnchor:root.widthAnchor].active = YES;
    [scroll.widthAnchor constraintEqualToAnchor:root.widthAnchor].active = YES;
    [scroll.heightAnchor constraintGreaterThanOrEqualToConstant:350].active = YES;
    return [self paddedPane:root];
}
- (NSView *)paddedPane:(NSStackView *)inside {
    auto *pane = [NSView new]; inside.translatesAutoresizingMaskIntoConstraints = NO; [pane addSubview:inside];
    [NSLayoutConstraint activateConstraints:@[[inside.leadingAnchor constraintEqualToAnchor:pane.leadingAnchor constant:20],
        [inside.trailingAnchor constraintEqualToAnchor:pane.trailingAnchor constant:-20],
        [inside.topAnchor constraintEqualToAnchor:pane.topAnchor constant:20],
        [inside.bottomAnchor constraintLessThanOrEqualToAnchor:pane.bottomAnchor constant:-20]]];
    return pane;
}
- (void)restoreSettings {
    auto *d = NSUserDefaults.standardUserDefaults;
    [d registerDefaults:@{@"source":@"desktop", @"fps":@30, @"hold":@13, @"timeout":@2000, @"priority":@"off", @"update":@"diff", @"encoding":@"pair", @"page":@3826, @"downloadPage":@3826}];
    select_value(sourcePopup, [d stringForKey:@"source"]); select_value(priorityPopup, [d stringForKey:@"priority"]);
    select_value(updatePopup, [d stringForKey:@"update"]); select_value(encodingPopup, [d stringForKey:@"encoding"]);
    fpsField.stringValue = ns(std::to_string([d integerForKey:@"fps"])); holdField.stringValue = ns(std::to_string([d integerForKey:@"hold"]));
    timeoutField.stringValue = ns(std::to_string([d integerForKey:@"timeout"])); pageField.stringValue = ns(std::to_string([d integerForKey:@"page"]));
    autoReconnectButton.state = [d boolForKey:@"reconnect"] ? NSControlStateValueOn : NSControlStateValueOff;
    imagePath = [d stringForKey:@"image"]; if (!imagePath) imagePath = @"";
    imageLabel.stringValue = imagePath.length ? imagePath.lastPathComponent : @"尚未選擇圖片";
    [self updateFlashRange];
    downloadPageField.stringValue = ns(std::to_string([d integerForKey:@"downloadPage"]));
    downloadFolder = [d stringForKey:@"downloadFolder"];
    downloadFolderLabel.stringValue = downloadFolder.length ? downloadFolder : @"尚未選擇下載資料夾";
    [self updateDownloadRange];
}
- (msu2app::Settings)settings {
    msu2app::Settings s;
    s.port = utf8(devicePopup.selectedItem.representedObject);
    s.source = utf8(sourcePopup.selectedItem.representedObject); s.image_path = utf8(imagePath);
    s.fps = unsigned(std::clamp<NSInteger>(fpsField.integerValue, 1, 60)); fpsField.stringValue = ns(std::to_string(s.fps));
    s.hold_open_ms = unsigned(std::clamp<NSInteger>(holdField.integerValue, 0, 1000)); holdField.stringValue = ns(std::to_string(s.hold_open_ms));
    s.timeout_ms = unsigned(std::clamp<NSInteger>(timeoutField.integerValue, 100, 10000)); timeoutField.stringValue = ns(std::to_string(s.timeout_ms));
    s.priority = utf8(priorityPopup.selectedItem.representedObject); s.differential = [updatePopup.selectedItem.representedObject isEqual:@"diff"];
    s.pair = [encodingPopup.selectedItem.representedObject isEqual:@"pair"]; s.auto_reconnect = autoReconnectButton.state == NSControlStateValueOn;
    return s;
}
- (void)saveSettings {
    if (smokeMode) return;
    auto s = [self settings]; auto *d = NSUserDefaults.standardUserDefaults;
    [d setObject:ns(s.port) forKey:@"port"]; [d setObject:ns(s.source) forKey:@"source"]; [d setObject:imagePath ? imagePath : @"" forKey:@"image"];
    [d setInteger:s.fps forKey:@"fps"]; [d setInteger:s.hold_open_ms forKey:@"hold"]; [d setInteger:s.timeout_ms forKey:@"timeout"];
    [d setObject:ns(s.priority) forKey:@"priority"]; [d setObject:s.differential ? @"diff" : @"full" forKey:@"update"];
    [d setObject:s.pair ? @"pair" : @"raw" forKey:@"encoding"]; [d setBool:s.auto_reconnect forKey:@"reconnect"]; [d setInteger:pageField.integerValue forKey:@"page"];
    if (valid_page(downloadPageField.stringValue)) [d setInteger:downloadPageField.integerValue forKey:@"downloadPage"];
    [d setObject:downloadFolder ? downloadFolder : @"" forKey:@"downloadFolder"];
}
- (void)settingsChanged:(id)sender { [self saveSettings]; if (sender == sourcePopup) [self updatePreview:nil]; }
- (void)controlTextDidEndEditing:(NSNotification *)notification { (void)notification; [self saveSettings]; [self updateFlashRange]; [self updateDownloadRange]; [self tick:nil]; }
- (void)deviceChanged:(id)sender { (void)sender; [self saveSettings]; [self updateDeviceHint]; }
- (void)updateDeviceHint {
    auto path = utf8(devicePopup.selectedItem.representedObject);
    auto it = std::find_if(devices.begin(), devices.end(), [&](const msu2app::Device& d) { return d.port == path; });
    if (it == devices.end()) deviceHint.stringValue = devices.empty()
        ? @"未找到可用的序列埠。請確認 USB 連線與裝置模式後重新整理；USB 名稱或 VID/PID 本身無法確認為 MSU2。"
        : @"請選擇候選序列埠。USB 名稱或 VID/PID 本身無法確認為 MSU2；連線時會執行協定握手。";
    else deviceHint.stringValue = [NSString stringWithFormat:@"%@%@ · 需通過 MSU2 協定握手", ns(it->port), it->serial.empty() ? @"" : [@" · 序號 " stringByAppendingString:ns(it->serial)]];
}
- (void)refreshDevices:(id)sender {
    if (smokeMode) { deviceHint.stringValue = @"離線介面驗證，不列舉 USB 裝置。"; return; }
    (void)sender; if (refreshing) return; refreshing = YES;
    NSString *wanted = devicePopup.selectedItem.representedObject;
    if (!wanted) wanted = [NSUserDefaults.standardUserDefaults stringForKey:@"port"];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        auto values = msu2app::enumerate_devices();
        dispatch_async(dispatch_get_main_queue(), ^{
            self->devices = values; [self->devicePopup removeAllItems];
            [self->devicePopup addItemWithTitle:values.empty() ? @"沒有可用的序列埠" : @"請選擇候選序列埠"];
            self->devicePopup.lastItem.representedObject = @"";
            for (const auto& d : values) { [self->devicePopup addItemWithTitle:ns(d.label)]; self->devicePopup.lastItem.representedObject = ns(d.port); }
            select_value(self->devicePopup, wanted);
            if (![self->devicePopup.selectedItem.representedObject length]) {
                const auto count = std::count_if(values.begin(), values.end(), [](const msu2app::Device& d) { return d.vendor == 0x1a86 && d.product == 0xfe0c; });
                if (count == 1) for (const auto& d : values) if (d.vendor == 0x1a86 && d.product == 0xfe0c) select_value(self->devicePopup, ns(d.port));
            }
            self->refreshing = NO; [self updateDeviceHint]; [self tick:nil];
        });
    });
}
- (void)showError:(NSString *)text {
    auto *alert = [NSAlert new]; alert.messageText = @"無法完成操作"; alert.informativeText = text; alert.alertStyle = NSAlertStyleWarning;
    [alert beginSheetModalForWindow:window completionHandler:nil];
}
- (BOOL)validSource:(const msu2app::Settings&)s {
    if (s.source == "image" && s.image_path.empty()) { [self showError:@"請先選擇圖片。"]; return NO; }
    if (s.source == "desktop" && !msu2app::capture_permission()) { [self showError:@"請先按「授權桌面擷取」，在系統設定允許本程式的螢幕錄製權限。圖片與內建圖樣不需要此權限。"]; return NO; }
    return YES;
}
- (void)connect:(id)sender { (void)sender; if (smokeMode) return; [self saveSettings]; auto s = [self settings]; if (s.port.empty()) return; pendingOperation = @"連線中"; controller->connect(s); [self tick:nil]; }
- (void)disconnect:(id)sender { (void)sender; if (smokeMode) return; pendingOperation = @"中斷連線中"; controller->disconnect(); [self tick:nil]; }
- (void)start:(id)sender { (void)sender; if (smokeMode) return; [self saveSettings]; auto s = [self settings]; if ([self validSource:s]) { pendingOperation = @"啟動串流中"; controller->start_stream(s); } [self tick:nil]; }
- (void)sendOne:(id)sender { (void)sender; if (smokeMode) return; [self saveSettings]; auto s = [self settings]; if ([self validSource:s]) { pendingOperation = @"傳送單張中"; controller->send_image(s); } [self tick:nil]; }
- (void)pause:(id)sender { (void)sender; if (smokeMode) return; controller->pause(!controller->snapshot().paused); [self tick:nil]; }
- (void)stop:(id)sender { (void)sender; if (smokeMode) return; pendingOperation = @"停止中"; controller->stop(); [self tick:nil]; }
- (void)requestPermission:(id)sender {
    if (smokeMode) return;
    (void)sender;
    if (msu2app::request_capture_permission()) previewHint.stringValue = @"螢幕錄製已允許。可按「更新預覽」或開始串流。";
    else {
        previewHint.stringValue = @"請在「系統設定 → 隱私權與安全性 → 螢幕與系統音訊錄製」允許本程式；必要時重新啟動。";
        [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture"]];
    }
}
- (void)pickImageForFlash:(BOOL)isFlash {
    auto *panel = [NSOpenPanel openPanel]; panel.canChooseDirectories = NO; panel.allowsMultipleSelection = NO;
    auto *ppmType = [UTType typeWithFilenameExtension:@"ppm"];
    panel.allowedContentTypes = @[UTTypePNG, UTTypeJPEG, ppmType ? ppmType : UTTypeData];
    panel.message = isFlash ? @"選擇要寫入外部 Flash 的圖片素材" : @"選擇要顯示的圖片；將中央裁切為 160 × 80";
    [panel beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
        if (response != NSModalResponseOK) return;
        if (isFlash) { self->confirmedFlashImage.reset(); [self->flashPreview setPreview:msu2::Image{}]; self->flashPath = panel.URL.path; self->flashPathLabel.stringValue = self->flashPath.lastPathComponent; [self loadFlashPreview]; }
        else { self->imagePath = panel.URL.path; self->imageLabel.stringValue = self->imagePath.lastPathComponent; select_value(self->sourcePopup, @"image"); [self saveSettings]; [self updatePreview:nil]; }
    }];
}
- (void)chooseImage:(id)sender { (void)sender; [self pickImageForFlash:NO]; }
- (void)chooseFlashImage:(id)sender { (void)sender; [self pickImageForFlash:YES]; }
- (void)updatePreview:(id)sender {
    if (smokeMode) { localPreview = msu2::make_pattern("bars"); [previewView setPreview:localPreview]; return; }
    (void)sender; auto s = [self settings]; const NSUInteger generation = ++previewGeneration;
    if (s.source == "desktop" && !msu2app::capture_permission()) { previewBusy = NO; sourceImageValid = NO; [previewView setPreview:msu2::Image{}]; previewHint.stringValue = @"桌面預覽尚未啟用。按「授權桌面擷取」後可更新預覽；圖片與內建圖樣可離線預覽。"; return; }
    if (s.source == "image" && s.image_path.empty()) { previewBusy = NO; sourceImageValid = NO; [previewView setPreview:msu2::Image{}]; previewHint.stringValue = @"請選擇一張圖片以預覽。"; return; }
    previewBusy = YES; sourceImageValid = NO;
    previewHint.stringValue = @"正在準備預覽…";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool {
            try {
                msu2::Image im;
                if (s.source == "desktop") { msu2app::ScreenCapture capture; im = capture.grab(); }
                else if (s.source == "image") im = msu2app::load_image(s.image_path);
                else im = msu2::make_pattern(s.source);
                dispatch_async(dispatch_get_main_queue(), ^{ if (generation != self->previewGeneration) return; self->previewBusy = NO; self->sourceImageValid = YES; self->localPreview = im; [self->previewView setPreview:im]; self->previewHint.stringValue = @"160 × 80 · 本機預覽，尚未傳送至裝置"; });
            } catch (const std::exception& e) {
                NSString *error = ns(e.what()); dispatch_async(dispatch_get_main_queue(), ^{ if (generation == self->previewGeneration) { self->previewBusy = NO; self->sourceImageValid = NO; [self->previewView setPreview:msu2::Image{}]; self->previewHint.stringValue = error; } });
            }
        }
    });
}
- (void)loadFlashPreview {
    NSString *path = flashPath; flashButton.enabled = NO;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        try {
            auto prepared = std::make_shared<const msu2::Image>(msu2app::load_image(utf8(path)));
            dispatch_async(dispatch_get_main_queue(), ^{ if (![path isEqual:self->flashPath]) return; self->confirmedFlashImage = prepared; [self->flashPreview setPreview:*prepared]; [self tick:nil]; });
        } catch (const std::exception& e) {
            NSString *error = ns(e.what()); dispatch_async(dispatch_get_main_queue(), ^{ if (![path isEqual:self->flashPath]) return; self->confirmedFlashImage.reset(); [self->flashPreview setPreview:msu2::Image{}]; self->flashPath = @""; [self showError:error]; });
        }
    });
}
- (void)updateFlashRange {
    const NSInteger page = pageField.integerValue;
    if (!valid_page(pageField.stringValue)) { flashRangeLabel.stringValue = @"起始頁必須為 0–3996 的整數；不能超出 4096 頁容量。"; return; }
    flashRangeLabel.stringValue = [NSString stringWithFormat:@"將覆寫第 %ld–%ld 頁（100 頁 / 25,600 bytes）", (long)page, (long)(page + 99)];
}
- (void)flashPreset:(NSPopUpButton *)sender { if ([sender.selectedItem.representedObject isEqual:@"custom"]) return; pageField.stringValue = sender.selectedItem.representedObject; [self updateFlashRange]; [self saveSettings]; }
- (void)flash:(id)sender {
    if (smokeMode) return;
    (void)sender; [window makeFirstResponder:nil]; [self updateFlashRange];
    const NSInteger page = pageField.integerValue;
    if (!valid_page(pageField.stringValue) || !flashPath.length || !confirmedFlashImage) { [self showError:@"請選擇可解碼的圖片，並輸入 0–3996 之間的整數起始頁。"]; return; }
    auto s = [self settings]; s.source = "image"; s.image_path = utf8(flashPath);
    s.prepared_image = confirmedFlashImage;
    auto *alert = [NSAlert new]; alert.alertStyle = NSAlertStyleCritical;
    alert.messageText = [NSString stringWithFormat:@"覆寫外部 Flash 第 %ld–%ld 頁？", (long)page, (long)(page + 99)];
    alert.informativeText = [NSString stringWithFormat:@"裝置：%@\n圖片：%@\n160 × 80 RGB565，25,600 bytes，100 頁。\n\n原有素材將被覆寫。程式會先將涵蓋的 4 KiB 區塊備份至下列紀錄資料夾，再寫入並讀回比對圖片及相鄰資料。\n\n%@\n\n實體 Flash 擦除與相鄰頁保留尚未完成硬體驗證；沒有自動還原功能。本操作不更新 MCU 韌體。", ns(s.port), flashPath, ns(controller->snapshot().log_directory)];
    [alert addButtonWithTitle:@"取消"]; [alert addButtonWithTitle:@"確認覆寫並燒錄"];
    [alert beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) { if (response == NSAlertSecondButtonReturn) { self->pendingOperation = @"準備燒錄中"; self->controller->flash_image(s, unsigned(page)); [self tick:nil]; } }];
}
- (void)updateDownloadRange {
    if (!valid_page(downloadPageField.stringValue)) { downloadRangeLabel.stringValue = @"起始頁必須為 0–3996 的整數。"; return; }
    const NSInteger page = downloadPageField.integerValue;
    downloadRangeLabel.stringValue = [NSString stringWithFormat:@"讀取第 %ld–%ld 頁（100 頁 / 25,600 bytes）", (long)page, (long)(page + 99)];
    select_value(downloadPreset, @"custom");
    select_value(downloadPreset, downloadPageField.stringValue);
}
- (void)downloadPreset:(NSPopUpButton *)sender {
    if ([sender.selectedItem.representedObject isEqual:@"custom"]) return;
    downloadPageField.stringValue = sender.selectedItem.representedObject; [self updateDownloadRange]; [self saveSettings]; [self tick:nil];
}
- (void)chooseDownloadFolder:(id)sender {
    (void)sender; if (smokeMode) return;
    auto *panel = [NSOpenPanel openPanel]; panel.canChooseFiles = NO; panel.canChooseDirectories = YES; panel.canCreateDirectories = YES; panel.allowsMultipleSelection = NO;
    panel.message = @"選擇圖片下載資料夾；每次下載會建立獨立的頁面資料夾。";
    if (downloadFolder.length) panel.directoryURL = [NSURL fileURLWithPath:downloadFolder isDirectory:YES];
    [panel beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
        if (response != NSModalResponseOK) return;
        self->downloadFolder = panel.URL.path; self->downloadFolderLabel.stringValue = self->downloadFolder; [self saveSettings]; [self tick:nil];
    }];
}
- (void)download:(id)sender {
    (void)sender; if (smokeMode) return; [window makeFirstResponder:nil]; [self updateDownloadRange];
    if (!valid_page(downloadPageField.stringValue) || !downloadFolder.length) { [self showError:@"請輸入 0–3996 的起始頁，並選擇下載資料夾。"]; return; }
    [self saveSettings]; downloadActive = YES; pendingOperation = @"準備讀取中";
    [downloadPreview setPreview:msu2::Image{}];
    controller->download_image([self settings], unsigned(downloadPageField.integerValue), utf8(downloadFolder)); [self tick:nil];
}
- (void)openDownload:(id)sender {
    (void)sender; if (smokeMode) return; auto path = controller->snapshot().download_directory;
    if (!path.empty()) [NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:ns(path) isDirectory:YES]];
}
- (void)openLogs:(id)sender { (void)sender; auto path = controller->snapshot().log_directory; if (!path.empty()) [NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:ns(path) isDirectory:YES]]; }
- (void)tick:(id)sender {
    (void)sender; if (!controller) return; auto s = controller->snapshot();
    if (smokeMode) {
        s.state = utf8(fixtureState ? fixtureState : @"disconnected");
        s.connected = s.state == "ready" || s.state == "starting" || s.state == "streaming" || s.state == "paused" || s.state == "stopping" || s.state == "downloading" || s.state == "downloaded";
        s.streaming = s.state == "streaming" || s.state == "paused" || s.state == "stopping";
        s.paused = s.state == "paused";
        s.busy = s.streaming || s.state == "connecting" || s.state == "starting" || s.state == "downloading";
        s.message = "離線介面狀態示意";
        s.download_progress = s.state == "downloading" ? 0.42 : (s.state == "downloaded" ? 1 : 0);
        s.download_bytes = std::size_t(s.download_progress * 25600);
        if (s.state == "downloaded") { s.download_preview = localPreview; s.download_directory = "下載資料夾 / MSU2_page3826-3925"; }
    }
    if (pendingOperation && (!s.busy || s.state == "error" || ([pendingOperation isEqual:@"啟動串流中"] && s.streaming) || ([pendingOperation isEqual:@"準備燒錄中"] && s.state == "flashing") || ([pendingOperation isEqual:@"準備讀取中"] && s.state == "downloading"))) pendingOperation = nil;
    NSString *state = @"尚未連線";
    if (s.state == "error") state = @"操作失敗";
    else if (s.state == "connecting") state = @"連線中";
    else if (s.state == "starting") state = @"啟動串流中";
    else if (s.state == "stopping") state = @"停止中";
    else if (s.state == "flashing") state = @"燒錄中";
    else if (s.state == "downloading") state = @"下載中";
    else if (s.paused) state = @"已暫停";
    else if (s.streaming) state = @"串流中";
    else if (s.connected) state = @"已連線";
    if (pendingOperation) state = pendingOperation;
    statusLabel.stringValue = [NSString stringWithFormat:@"%@ · %@%@", state, ns(s.message), s.firmware_banner >= 0 ? [NSString stringWithFormat:@" · 韌體識別 %d", s.firmware_banner] : @""];
    statusLabel.textColor = s.state == "error" ? NSColor.systemRedColor : (s.connected ? NSColor.systemBlueColor : NSColor.secondaryLabelColor);
    const BOOL idle = !s.busy && !s.streaming && !pendingOperation;
    const BOOL sourceReady = smokeMode || (utf8(sourcePopup.selectedItem.representedObject) == "desktop" ? msu2app::capture_permission() : (utf8(sourcePopup.selectedItem.representedObject) == "image" ? sourceImageValid && !previewBusy : YES));
    connectButton.enabled = !s.connected && idle && utf8(devicePopup.selectedItem.representedObject).size() > 0;
    disconnectButton.enabled = s.connected && s.state != "flashing" && !pendingOperation;
    devicePopup.enabled = !s.connected && !s.busy;
    startButton.enabled = s.connected && idle && sourceReady; sendButton.enabled = s.connected && idle && sourceReady;
    pauseButton.enabled = s.streaming && !pendingOperation && s.state != "stopping"; pauseButton.title = s.paused ? @"繼續" : @"暫停";
    stopButton.enabled = s.streaming && !pendingOperation && s.state != "stopping";
    flashButton.enabled = s.connected && idle && confirmedFlashImage && valid_page(pageField.stringValue);
    flashCancelButton.enabled = s.state == "flashing" && !pendingOperation;
    flashProgress.doubleValue = s.flash_progress;
    const BOOL downloading = s.state == "downloading" || (downloadActive && s.busy);
    downloadButton.enabled = s.connected && idle && valid_page(downloadPageField.stringValue) && downloadFolder.length;
    downloadCancelButton.enabled = downloading && !pendingOperation;
    downloadFolderButton.enabled = idle; downloadPreset.enabled = idle; downloadPageField.enabled = idle;
    downloadOpenButton.enabled = !s.download_directory.empty() && !downloading;
    downloadProgress.doubleValue = s.download_progress;
    if (downloading) {
        [downloadPreview setPreview:msu2::Image{}];
        downloadStatusLabel.stringValue = [NSString stringWithFormat:@"正在讀取：%lu / 25,600 bytes（%.0f%%）", (unsigned long)s.download_bytes, s.download_progress * 100];
    } else if (!s.download_directory.empty()) {
        [downloadPreview setPreview:s.download_preview];
        downloadStatusLabel.stringValue = [NSString stringWithFormat:@"下載完成：%@\n已保存 image.png、image.rgb565 與 manifest.txt。", ns(s.download_directory)];
    } else if (downloadActive && !s.busy) { [downloadPreview setPreview:msu2::Image{}]; downloadStatusLabel.stringValue = ns(s.message); }
    if (downloadActive && !s.busy) downloadActive = NO;
    rateMetric.stringValue = [NSString stringWithFormat:@"來源取得  %.1f FPS  /  主機更新  %.1f 次/秒\nLCD 實際更新率：未量測", s.capture_fps, s.host_fps];
    framesMetric.stringValue = [NSString stringWithFormat:@"傳送  %llu  /  未變更  %llu\n等待影格取代  %llu  /  來源丟幀  %llu", (unsigned long long)s.sent, (unsigned long long)s.unchanged, (unsigned long long)s.queue_dropped, (unsigned long long)s.capture_dropped];
    errorsMetric.stringValue = [NSString stringWithFormat:@"ACK 逾時  %llu  /  握手失敗  %llu\n傳輸錯誤  %llu  /  重連  %llu", (unsigned long long)s.ack_timeouts, (unsigned long long)s.handshake_failures, (unsigned long long)s.transport_errors, (unsigned long long)s.reconnects];
    latencyMetric.stringValue = [NSString stringWithFormat:@"ACK  %.1f ms\n寫入  %.1f ms", s.ack_ms, s.write_ms];
    if (!s.preview.rgb.empty() && (s.streaming || s.busy)) { [previewView setPreview:s.preview]; previewHint.stringValue = [NSString stringWithFormat:@"目前來源預覽 · 區域 %@ · 優先 %@", ns(s.region), ns(s.active_layout)]; }
    [previewView updateBorder]; [flashPreview updateBorder]; [downloadPreview updateBorder];
    std::string log;
    for (const auto& line : s.logs) { log += line; log += '\n'; }
    if (log != priorLog) { priorLog = log; historyText.string = ns(log); [historyText scrollRangeToVisible:NSMakeRange(historyText.string.length, 0)]; }
    if (quitPending && !s.busy && !s.streaming) { quitPending = NO; [NSApp replyToApplicationShouldTerminate:YES]; }
}
- (void)runQACase:(NSUInteger)index {
    NSArray *cases = @[
        @{ @"name": @"light_stream_bars", @"tab": @0, @"image": @"bars", @"state": @"disconnected" },
        @{ @"name": @"light_stream_black", @"tab": @0, @"image": @"black", @"state": @"ready" },
        @{ @"name": @"light_stream_white", @"tab": @0, @"image": @"white", @"state": @"streaming" },
        @{ @"name": @"light_stream_mixed", @"tab": @0, @"image": @"horizontal", @"state": @"paused" },
        @{ @"name": @"light_stream_edge", @"tab": @0, @"image": @"edge", @"state": @"error" },
        @{ @"name": @"light_flash", @"tab": @1, @"image": @"white", @"state": @"ready" },
        @{ @"name": @"light_download_ready", @"tab": @2, @"image": @"bars", @"state": @"ready" },
        @{ @"name": @"light_download_reading", @"tab": @2, @"image": @"bars", @"state": @"downloading" },
        @{ @"name": @"light_download_complete", @"tab": @2, @"image": @"bars", @"state": @"downloaded" },
        @{ @"name": @"light_history", @"tab": @3, @"image": @"bars", @"state": @"disconnected" }
    ];
    if (index >= cases.count) { if (!qaFailed) std::printf("UI_MATRIX_OK %lu offline cases complete\n", (unsigned long)cases.count); [NSApp terminate:nil]; return; }
    NSDictionary *testCase = cases[index];
    fixtureName = testCase[@"image"]; fixtureState = testCase[@"state"];
    localPreview = fixture(fixtureName); [previewView setPreview:localPreview]; [flashPreview setPreview:localPreview];
    [tabs selectTabViewItemAtIndex:[testCase[@"tab"] integerValue]];
    [self tick:nil];
    snapshotPath = [matrixPath stringByAppendingPathComponent:[testCase[@"name"] stringByAppendingPathExtension:@"png"]];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 350 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        [self writeSnapshot]; std::printf("UI_CASE_OK %s\n", [testCase[@"name"] UTF8String]); std::fflush(stdout); [self runQACase:index + 1];
    });
}
- (void)writeSnapshot {
    [window.contentView layoutSubtreeIfNeeded];
    auto *rep = [window.contentView bitmapImageRepForCachingDisplayInRect:window.contentView.bounds];
    [window.contentView cacheDisplayInRect:window.contentView.bounds toBitmapImageRep:rep];
    CGColorSpaceRef colors = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(nullptr, rep.pixelsWide, rep.pixelsHigh, 8, rep.pixelsWide * 4, colors, kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(colors);
    CGRect bounds = CGRectMake(0, 0, rep.pixelsWide, rep.pixelsHigh);
    CGContextSetRGBFillColor(context, 0.965, 0.965, 0.965, 1); CGContextFillRect(context, bounds);
    CGContextDrawImage(context, bounds, rep.CGImage);
    CGImageRef rendered = CGBitmapContextCreateImage(context);
    auto *flat = [[NSBitmapImageRep alloc] initWithCGImage:rendered];
    CGImageRelease(rendered); CGContextRelease(context);
    NSData *data = [flat representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    if (![data writeToFile:snapshotPath atomically:YES]) { qaFailed = YES; std::fprintf(stderr, "Failed to write UI snapshot\n"); }
    MSU2PreviewView *checked = tabs.selectedTabViewItem == [tabs tabViewItemAtIndex:1] ? flashPreview : (tabs.selectedTabViewItem == [tabs tabViewItemAtIndex:2] ? downloadPreview : previewView);
    const auto bytesBefore = [checked sourceBytes];
    NSString *borderChoice = checked.borderDescription;
    BOOL contrastBefore = checked.autoContrast;
    checked.autoContrast = !contrastBefore;
    const auto bytesAfter = [checked sourceBytes];
    checked.autoContrast = contrastBefore;
    NSDictionary *metadata = @{
        @"appearance": @"light", @"tab": @([tabs indexOfTabViewItem:tabs.selectedTabViewItem]),
        @"fixture": fixtureName ? fixtureName : @"bars", @"state": fixtureState ? fixtureState : @"disconnected",
        @"border": borderChoice, @"autoContrast": @(checked.autoContrast),
        @"payloadUnchanged": @(bytesBefore == bytesAfter), @"buttonHeight": @(startButton.bounds.size.height),
        @"startEnabled": @(startButton.enabled), @"pauseEnabled": @(pauseButton.enabled),
        @"downloadEnabled": @(downloadButton.enabled), @"downloadCancelEnabled": @(downloadCancelButton.enabled),
        @"downloadOpenEnabled": @(downloadOpenButton.enabled), @"downloadProgress": @(downloadProgress.doubleValue)
    };
    NSData *json = [NSJSONSerialization dataWithJSONObject:metadata options:NSJSONWritingPrettyPrinted error:nil];
    if (![json writeToFile:[[snapshotPath stringByDeletingPathExtension] stringByAppendingPathExtension:@"json"] atomically:YES]) { qaFailed = YES; std::fprintf(stderr, "Failed to write UI metadata\n"); }
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender { (void)sender; return YES; }
- (BOOL)windowShouldClose:(NSWindow *)sender { (void)sender; [NSApp terminate:nil]; return NO; }
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
    if (!controller) return NSTerminateNow;
    (void)sender; if (!smokeMode) [self saveSettings];
    const auto s = controller->snapshot();
    if (s.state == "flashing") { [self showError:@"圖片燒錄仍在進行，請等待完成後再結束程式。"]; return NSTerminateCancel; }
    if (s.busy || s.streaming) { quitPending = YES; controller->stop(); controller->disconnect(); return NSTerminateLater; }
    return NSTerminateNow;
}
- (void)applicationWillTerminate:(NSNotification *)notification { (void)notification; [timer invalidate]; controller.reset(); }
@end

int main(int argc, char **argv) {
    @autoreleasepool {
        [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSApp.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
        auto *delegate = [MSU2AppDelegate new];
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "--ui-smoke") delegate.smokeMode = YES;
            else if (std::string(argv[i]) == "--snapshot" && i + 1 < argc) delegate.snapshotPath = ns(argv[++i]);
            else if (std::string(argv[i]) == "--snapshot-tab" && i + 1 < argc) delegate.snapshotTab = ns(argv[++i]);
            else if (std::string(argv[i]) == "--ui-width" && i + 1 < argc) delegate.requestedWidth = std::stod(argv[++i]);
            else if (std::string(argv[i]) == "--ui-height" && i + 1 < argc) delegate.requestedHeight = std::stod(argv[++i]);
            else if (std::string(argv[i]) == "--ui-image" && i + 1 < argc) delegate.fixtureName = ns(argv[++i]);
            else if (std::string(argv[i]) == "--ui-state" && i + 1 < argc) delegate.fixtureState = ns(argv[++i]);
            else if (std::string(argv[i]) == "--ui-matrix" && i + 1 < argc) { delegate.matrixPath = ns(argv[++i]); delegate.smokeMode = YES; }
            else if (std::string(argv[i]) == "--log-root" && i + 1 < argc) delegate.logRoot = ns(argv[++i]);
        }
        NSApp.delegate = delegate; [NSApp run];
        return delegate.qaFailed ? 1 : 0;
    }
}
