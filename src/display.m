// LeapVM：在 Mac 上開一個視窗，顯示虛擬機的螢幕
//
// 虛擬機的螢幕其實就是它記憶體裡的一塊（每個像素 4 bytes）。
// Linux 把圖畫進這塊記憶體；我們每秒 60 次去看它有沒有變，有變就拿來畫在視窗上。
// 視窗裡的鍵盤和滑鼠動作，轉成 Linux 的輸入事件送給虛擬鍵盤、虛擬滑鼠（virtio_input.c）。
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <ImageIO/ImageIO.h>
#include "vm.h"

static bool       enabled;
static NSWindow  *window;
static CALayer   *screen;
static uint8_t   *shown;      // 目前視窗上的畫面（主機端的副本）

static size_t fb_bytes(void) { return (size_t)vm.fb_width * vm.fb_height * 4; }

static CGImageRef make_image(const uint8_t *pixels) {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CFDataRef data = CFDataCreate(NULL, pixels, (CFIndex)fb_bytes());
    CGDataProviderRef dp = CGDataProviderCreateWithCFData(data);
    // 記憶體裡是 藍、綠、紅、(不用) 的順序 = 32 位元小端序、第一個 byte 以外沒有透明度
    CGImageRef img = CGImageCreate(vm.fb_width, vm.fb_height, 8, 32, vm.fb_width * 4, cs,
                                   kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst, dp, NULL, false,
                                   kCGRenderingIntentDefault);
    CGDataProviderRelease(dp);
    CFRelease(data);
    CGColorSpaceRelease(cs);
    return img;
}

bool display_save_png(const char *path) {
    if (!vm.fb_width) return false;
    uint8_t *copy = malloc(fb_bytes());
    if (!copy) return false;
    memcpy(copy, vm.ram + vm.fb_offset, fb_bytes());
    CGImageRef img = make_image(copy);
    free(copy);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    bool ok = false;
    if (dst) {
        CGImageDestinationAddImage(dst, img, NULL);
        ok = CGImageDestinationFinalize(dst);
        CFRelease(dst);
    }
    CFRelease(url);
    CGImageRelease(img);
    return ok;
}

// 畫面半秒沒變，就從每秒檢查 60 次降到 10 次（每次比對 4MB，省下 CPU 和電）；
// 一有鍵盤滑鼠動作、或畫面一變，馬上回到每秒 60 次
static int idle_frames;

static void display_poke(void) { idle_frames = 0; }

static void refresh(void) {
    static unsigned tick;
    if (idle_frames > 30 && ++tick % 6) return;
    const uint8_t *src = vm.ram + vm.fb_offset;
    if (!memcmp(shown, src, fb_bytes())) {  // 沒變就不重畫
        idle_frames++;
        return;
    }
    idle_frames = 0;
    memcpy(shown, src, fb_bytes());
    CGImageRef img = make_image(shown);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];  // 關掉淡入淡出，每一格直接換上去
    screen.contents = (__bridge id)img;
    [CATransaction commit];
    CGImageRelease(img);
}

// ── 鍵盤：Mac 的按鍵代碼 → Linux 的按鍵代碼 ──
// 左邊是 Mac 的 kVK_* 代碼（依美式鍵盤位置），右邊是 Linux input-event-codes.h 的 KEY_*
static const uint8_t keymap[128] = {
    [0x00] = 30, [0x01] = 31, [0x02] = 32, [0x03] = 33, [0x04] = 35, [0x05] = 34,   // A S D F H G
    [0x06] = 44, [0x07] = 45, [0x08] = 46, [0x09] = 47, [0x0a] = 86, [0x0b] = 48,   // Z X C V § B
    [0x0c] = 16, [0x0d] = 17, [0x0e] = 18, [0x0f] = 19, [0x10] = 21, [0x11] = 20,   // Q W E R Y T
    [0x12] = 2, [0x13] = 3, [0x14] = 4, [0x15] = 5, [0x16] = 7, [0x17] = 6,         // 1 2 3 4 6 5
    [0x18] = 13, [0x19] = 10, [0x1a] = 8, [0x1b] = 12, [0x1c] = 9, [0x1d] = 11,     // = 9 7 - 8 0
    [0x1e] = 27, [0x1f] = 24, [0x20] = 22, [0x21] = 26, [0x22] = 23, [0x23] = 25,   // ] O U [ I P
    [0x24] = 28, [0x25] = 38, [0x26] = 36, [0x27] = 40, [0x28] = 37, [0x29] = 39,   // Enter L J ' K ;
    [0x2a] = 43, [0x2b] = 51, [0x2c] = 53, [0x2d] = 49, [0x2e] = 50, [0x2f] = 52,   // \ , / N M .
    [0x30] = 15, [0x31] = 57, [0x32] = 41, [0x33] = 14, [0x35] = 1,                 // Tab 空白 ` 退格 Esc
    [0x36] = 126, [0x37] = 125, [0x38] = 42, [0x39] = 58, [0x3a] = 56, [0x3b] = 29, // 右⌘ ⌘ Shift Caps ⌥ Ctrl
    [0x3c] = 54, [0x3d] = 100, [0x3e] = 97,                                         // 右 Shift、右 ⌥、右 Ctrl
    [0x41] = 83, [0x43] = 55, [0x45] = 78, [0x47] = 69, [0x4b] = 98, [0x4c] = 96,   // 數字鍵盤
    [0x4e] = 74, [0x51] = 117, [0x52] = 82, [0x53] = 79, [0x54] = 80, [0x55] = 81,
    [0x56] = 75, [0x57] = 76, [0x58] = 77, [0x59] = 71, [0x5b] = 72, [0x5c] = 73,
    [0x48] = 115, [0x49] = 114, [0x4a] = 113,                                       // 音量 + - 靜音
    [0x7a] = 59, [0x78] = 60, [0x63] = 61, [0x76] = 62, [0x60] = 63, [0x61] = 64,   // F1–F6
    [0x62] = 65, [0x64] = 66, [0x65] = 67, [0x6d] = 68, [0x67] = 87, [0x6f] = 88,   // F7–F12
    [0x72] = 110, [0x73] = 102, [0x74] = 104, [0x75] = 111, [0x77] = 107, [0x79] = 109,  // Insert Home PgUp Del End PgDn
    [0x7b] = 105, [0x7c] = 106, [0x7d] = 108, [0x7e] = 103,                         // ← → ↓ ↑
};

// 修飾鍵（Shift、Ctrl……）沒有 keyDown / keyUp，只有「狀態改變」，要看旗標判斷是按下還是放開
static bool modifier_down(uint16_t key, NSEventModifierFlags flags) {
    switch (key) {
    case 0x38: return flags & 0x02;      // 左 Shift
    case 0x3c: return flags & 0x04;      // 右 Shift
    case 0x3b: return flags & 0x01;      // 左 Ctrl
    case 0x3e: return flags & 0x2000;    // 右 Ctrl
    case 0x3a: return flags & 0x20;      // 左 Option
    case 0x3d: return flags & 0x40;      // 右 Option
    case 0x37: return flags & 0x08;      // 左 Command
    case 0x36: return flags & 0x10;      // 右 Command
    }
    return false;
}

// ── 視窗 ──
@interface ScreenView : NSView
@end

@implementation ScreenView

- (BOOL)acceptsFirstResponder { return YES; }

- (void)keyDown:(NSEvent *)event {
    display_poke();
    if (event.isARepeat) return;  // 按住不放的連續輸入交給 Linux 自己處理
    if (event.keyCode < 128) input_key(keymap[event.keyCode], true);
}

- (void)keyUp:(NSEvent *)event {
    display_poke();
    if (event.keyCode < 128) input_key(keymap[event.keyCode], false);
}

- (void)flagsChanged:(NSEvent *)event {
    display_poke();
    uint16_t k = event.keyCode;
    if (k >= 128 || !keymap[k]) return;
    if (k == 0x39) {  // Caps Lock：每次切換都當成按一下
        input_key(keymap[k], true);
        input_key(keymap[k], false);
        return;
    }
    input_key(keymap[k], modifier_down(k, event.modifierFlags));
}

// Command + 某些鍵本來會被 macOS 當成選單快捷鍵；除了 ⌘Q（結束）以外都送給 Linux
- (BOOL)performKeyEquivalent:(NSEvent *)event {
    if ((event.modifierFlags & NSEventModifierFlagCommand) && [event.charactersIgnoringModifiers isEqualToString:@"q"])
        return NO;
    if (event.type == NSEventTypeKeyDown) {
        [self keyDown:event];
        return YES;
    }
    return NO;
}

// 畫面在視窗裡的實際位置（等比例縮放、置中，旁邊可能有黑邊）
- (NSRect)screenRect {
    NSRect b = self.bounds;
    double scale = MIN(b.size.width / vm.fb_width, b.size.height / vm.fb_height);
    double w = vm.fb_width * scale, h = vm.fb_height * scale;
    return NSMakeRect((b.size.width - w) / 2, (b.size.height - h) / 2, w, h);
}

- (void)movePointer:(NSEvent *)event {
    display_poke();
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSRect r = [self screenRect];
    input_pointer_move((p.x - r.origin.x) / r.size.width, 1.0 - (p.y - r.origin.y) / r.size.height);
}

- (void)mouseMoved:(NSEvent *)e { [self movePointer:e]; }
- (void)mouseDragged:(NSEvent *)e { [self movePointer:e]; }
- (void)rightMouseDragged:(NSEvent *)e { [self movePointer:e]; }
- (void)otherMouseDragged:(NSEvent *)e { [self movePointer:e]; }
- (void)mouseDown:(NSEvent *)e { [self movePointer:e]; input_pointer_button(0, true); }
- (void)mouseUp:(NSEvent *)e { display_poke(); input_pointer_button(0, false); }
- (void)rightMouseDown:(NSEvent *)e { [self movePointer:e]; input_pointer_button(1, true); }
- (void)rightMouseUp:(NSEvent *)e { display_poke(); input_pointer_button(1, false); }
- (void)otherMouseDown:(NSEvent *)e { [self movePointer:e]; input_pointer_button(2, true); }
- (void)otherMouseUp:(NSEvent *)e { input_pointer_button(2, false); }

- (void)scrollWheel:(NSEvent *)event {
    display_poke();
    // 觸控板的捲動量很細，累積到一格才送；方向跟 Mac 的「自然捲動」一致
    static double acc_y, acc_x;
    acc_y += event.scrollingDeltaY * (event.hasPreciseScrollingDeltas ? 0.1 : 1);
    acc_x += event.scrollingDeltaX * (event.hasPreciseScrollingDeltas ? 0.1 : 1);
    int dy = (int)acc_y, dx = (int)acc_x;
    acc_y -= dy;
    acc_x -= dx;
    input_pointer_wheel(dy, -dx);
}

// Linux 會畫自己的游標，所以在畫面上把 Mac 的游標藏起來，免得看到兩個
- (void)resetCursorRects {
    static NSCursor *blank;
    if (!blank) {
        NSImage *img = [[NSImage alloc] initWithSize:NSMakeSize(1, 1)];
        blank = [[NSCursor alloc] initWithImage:img hotSpot:NSZeroPoint];
    }
    [self addCursorRect:[self screenRect] cursor:blank];
}

- (void)updateTrackingAreas {
    for (NSTrackingArea *a in self.trackingAreas) [self removeTrackingArea:a];
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                       options:NSTrackingMouseMoved | NSTrackingActiveInKeyWindow |
                                                               NSTrackingInVisibleRect
                                                         owner:self
                                                      userInfo:nil]];
    [super updateTrackingAreas];
}
@end

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@implementation AppDelegate
// 關視窗或按 ⌘Q：不要直接切掉（就像拔電源，Linux 還沒寫進硬碟的資料會不見），
// 而是按一下虛擬的「電源鍵」，讓 Linux 自己好好關機；它關完會通知我們結束。
// 如果 Linux 卡住 30 秒都沒關，再按一次 ⌘Q 就會強制結束。
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
    (void)sender;
    static bool asked;
    if (asked) {
        LOG("強制結束虛擬機\r\n");
        vm_exit(0);
    }
    asked = true;
    LOG("按下電源鍵，等 Linux 關機……（再按一次 ⌘Q 可以強制結束）\r\n");
    window.title = @"LeapVM — 正在關機……";
    input_key(116, true);   // KEY_POWER
    input_key(116, false);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        LOG("Linux 30 秒內沒有關機，強制結束\r\n");
        vm_exit(0);
    });
    return NSTerminateCancel;
}
- (BOOL)windowShouldClose:(NSWindow *)sender {
    (void)sender;
    [NSApp terminate:nil];
    return NO;  // 視窗等 Linux 關機完才消失
}
@end

static AppDelegate *delegate;

static void build_menu(void) {
    NSMenu *bar = [NSMenu new];
    NSMenuItem *app = [NSMenuItem new];
    [bar addItem:app];
    NSMenu *menu = [NSMenu new];
    [menu addItemWithTitle:@"結束 LeapVM" action:@selector(terminate:) keyEquivalent:@"q"];
    app.submenu = menu;
    NSApp.mainMenu = bar;
}

_Noreturn void display_run(void *(*vm_main)(void *)) {
    enabled = true;
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        delegate = [AppDelegate new];
        NSApp.delegate = delegate;
        build_menu();
        pthread_t t;
        pthread_create(&t, NULL, vm_main, NULL);
        pthread_detach(t);
        [NSApp run];
    }
    exit(0);
}

void display_attach(void) {
    if (!enabled || !vm.fb_width) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (window) return;
        shown = calloc(1, fb_bytes());
        // 視窗預設是畫面的一半大小（Retina 螢幕上一個點 = 兩個像素，看起來剛好一比一）
        NSRect frame = NSMakeRect(0, 0, vm.fb_width / 2.0, vm.fb_height / 2.0);
        window = [[NSWindow alloc] initWithContentRect:frame
                                             styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                       NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
        window.title = [NSString stringWithFormat:@"LeapVM — 在「%@」上執行", NSHost.currentHost.localizedName];
        window.delegate = delegate;
        window.contentAspectRatio = frame.size;  // 拉大縮小時維持比例
        window.releasedWhenClosed = NO;

        ScreenView *view = [[ScreenView alloc] initWithFrame:frame];
        view.wantsLayer = YES;
        screen = view.layer;
        screen.backgroundColor = NSColor.blackColor.CGColor;
        screen.contentsGravity = kCAGravityResizeAspect;
        screen.magnificationFilter = kCAFilterNearest;  // 放大時保持像素清楚，不要糊掉
        screen.minificationFilter = kCAFilterTrilinear;
        window.contentView = view;
        [window center];
        window.acceptsMouseMovedEvents = YES;
        [window makeKeyAndOrderFront:nil];
        [window makeFirstResponder:view];
        [NSApp activate];

        [NSTimer scheduledTimerWithTimeInterval:1.0 / 60 repeats:YES block:^(NSTimer *timer) {
            (void)timer;
            refresh();
        }];
        refresh();
        if (getenv("LEAPVM_DEBUG")) LOG("視窗編號 %ld\r\n", (long)window.windowNumber);
    });
}
