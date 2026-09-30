// LeapVM 測試動畫：在虛擬機的螢幕上畫彈跳的球和畫格計數器。
// 只給自動化測試用（tests/test_display.py），確認「Linux 畫的東西會出現在視窗裡、而且會更新」。
//
// 螢幕的位置：LeapVM 在開機參數裡加上 leapvm.fb=<實體位址>,<寬>,<高>。
// 這個 Alpine 核心沒有內建顯示驅動，所以我們透過 /dev/mem 直接把那塊記憶體對應進來寫。
//
// 不用 C 函式庫，直接用 Linux 系統呼叫，所以可以用 bench/mkelf.py 組成執行檔。
#include <stddef.h>
#include <stdint.h>

#define INLINE static inline __attribute__((always_inline))

INLINE long sys6(long n, long a, long b, long c, long d, long e, long f) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
    return x0;
}

enum { SYS_openat = 56, SYS_read = 63, SYS_write = 64, SYS_close = 57, SYS_mmap = 222, SYS_nanosleep = 101,
       SYS_clock_gettime = 113, SYS_exit = 93 };

// 編譯器有時會把迴圈自動換成 memcpy / memset 呼叫，所以自己提供一份
void *memset(void *d, int c, size_t n) {
    uint8_t *p = d;
    for (size_t i = 0; i < n; i++) { p[i] = (uint8_t)c; __asm__ volatile("" ::: "memory"); }
    return d;
}

void *memcpy(void *d, const void *s, size_t n) {
    uint8_t *p = d;
    const uint8_t *q = s;
    for (size_t i = 0; i < n; i++) { p[i] = q[i]; __asm__ volatile("" ::: "memory"); }
    return d;
}

static uint64_t now_ns(void) {
    long ts[2];
    sys6(SYS_clock_gettime, 1 /* CLOCK_MONOTONIC */, (long)ts, 0, 0, 0, 0);
    return (uint64_t)ts[0] * 1000000000ULL + (uint64_t)ts[1];
}

static void sleep_ns(uint64_t ns) {
    long ts[2] = {(long)(ns / 1000000000ULL), (long)(ns % 1000000000ULL)};
    sys6(SYS_nanosleep, (long)ts, 0, 0, 0, 0, 0);
}

// ── 5×7 點陣字 ──
static const uint8_t font[][7] = {
    {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}, {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e},  // 0 1
    {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}, {0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e},  // 2 3
    {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}, {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e},  // 4 5
    {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e}, {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},  // 6 7
    {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}, {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c},  // 8 9
    {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}, {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f},  // A E
    {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}, {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e},  // F I
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f}, {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11},  // L M
    {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}, {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11},  // P R
    {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}, {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},  // S T
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c},  // V .
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},                                              // 空白
};

static int glyph(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    switch (c) {
    case 'A': return 10; case 'E': return 11; case 'F': return 12; case 'I': return 13;
    case 'L': return 14; case 'M': return 15; case 'P': return 16; case 'R': return 17;
    case 'S': return 18; case 'T': return 19; case 'V': return 20; case '.': return 21;
    }
    return 22;
}

static uint32_t *px;  // 畫在這塊（一般記憶體），畫完再把有變的列搬到螢幕
static int W, H;

static void fill_rect(int x, int y, int w, int h, uint32_t color) {
    for (int j = y < 0 ? 0 : y; j < y + h && j < H; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < W; i++) px[j * W + i] = color;
}

static void draw_text(int x, int y, int scale, const char *s, uint32_t color) {
    for (; *s; s++, x += 6 * scale) {
        const uint8_t *g = font[glyph(*s)];
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++)
                if (g[r] >> (4 - c) & 1) fill_rect(x + c * scale, y + r * scale, scale, scale, color);
    }
}

static void fill_circle(int cx, int cy, int r, uint32_t color) {
    for (int dy = -r; dy <= r; dy++) {
        int y = cy + dy;
        if (y < 0 || y >= H) continue;
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r && cx + dx >= 0 && cx + dx < W) px[y * W + cx + dx] = color;
    }
}

// 數字轉成固定位數的文字
static char *fmt_num(char *out, uint64_t v, int digits) {
    for (int i = digits - 1; i >= 0; i--) { out[i] = (char)('0' + v % 10); v /= 10; }
    return out + digits;
}

// 解析開機參數裡的 leapvm.fb=0x5fe00000,800,600
static int parse_fb(const char *s, uint64_t *addr, int *w, int *h) {
    const char *key = "leapvm.fb=";
    for (; *s; s++) {
        const char *a = s, *b = key;
        while (*b && *a == *b) { a++; b++; }
        if (*b) continue;
        uint64_t v[3] = {0, 0, 0};
        int k = 0;
        if (a[0] == '0' && a[1] == 'x') a += 2;
        for (; *a && *a != ' ' && *a != '\n' && k < 3; a++) {
            if (*a == ',') { k++; continue; }
            int d = *a >= '0' && *a <= '9' ? *a - '0' : *a >= 'a' && *a <= 'f' ? *a - 'a' + 10 : -1;
            if (d < 0) return 0;
            v[k] = v[k] * (k == 0 ? 16 : 10) + (uint64_t)d;
        }
        *addr = v[0];
        *w = (int)v[1];
        *h = (int)v[2];
        return k == 2;
    }
    return 0;
}

#define NBALLS 7
#define FP 256  // 定點數：位置和速度都乘上 256，避免用浮點數

void _start(void) {
    char cmdline[1024];
    int fd = (int)sys6(SYS_openat, -100, (long)"/proc/cmdline", 0, 0, 0, 0);
    long n = fd >= 0 ? sys6(SYS_read, fd, (long)cmdline, sizeof cmdline - 1, 0, 0, 0) : -1;
    cmdline[n > 0 ? n : 0] = 0;
    uint64_t fb_addr;
    if (!parse_fb(cmdline, &fb_addr, &W, &H)) sys6(SYS_exit, 1, 0, 0, 0, 0, 0);  // 沒有螢幕
    int stride = W;
    fd = (int)sys6(SYS_openat, -100, (long)"/dev/mem", 2 | 0x101000 /* O_RDWR | O_SYNC */, 0, 0, 0);
    if (fd < 0) sys6(SYS_exit, 2, 0, 0, 0, 0, 0);
    uint32_t *fb = (uint32_t *)sys6(SYS_mmap, 0, (long)W * H * 4, 3, 1 /* MAP_SHARED */, fd, (long)fb_addr);
    if ((long)fb < 0 && (long)fb > -4096) sys6(SYS_exit, 3, 0, 0, 0, 0, 0);
    px = (uint32_t *)sys6(SYS_mmap, 0, (long)W * H * 4, 3, 0x22, -1, 0);
    uint32_t *shown = (uint32_t *)sys6(SYS_mmap, 0, (long)W * H * 4, 3, 0x22, -1, 0);

    static const uint32_t colors[NBALLS] = {0xffe5484d, 0xfff5a623, 0xfff8e71c, 0xff4cd964,
                                            0xff34aadc, 0xff5856d6, 0xffff2d95};
    int32_t x[NBALLS], y[NBALLS], vx[NBALLS], vy[NBALLS], r[NBALLS];
    for (int i = 0; i < NBALLS; i++) {
        r[i] = 18 + i * 5;
        x[i] = (60 + i * 100) * FP;
        y[i] = (140 + (i * 53) % 200) * FP;
        vx[i] = (i % 2 ? 1 : -1) * (180 + i * 40);
        vy[i] = 0;
    }

    uint64_t start = now_ns(), next = start;
    for (uint64_t frame = 1;; frame++) {
        // 物理：重力 + 碰到牆壁反彈（完全彈性，會一直彈下去）
        for (int i = 0; i < NBALLS; i++) {
            vy[i] += 40;
            x[i] += vx[i];
            y[i] += vy[i];
            if (x[i] < r[i] * FP) { x[i] = r[i] * FP; vx[i] = -vx[i]; }
            if (x[i] > (W - r[i]) * FP) { x[i] = (W - r[i]) * FP; vx[i] = -vx[i]; }
            if (y[i] > (H - r[i]) * FP) { y[i] = (H - r[i]) * FP; vy[i] = -vy[i]; }
        }

        fill_rect(0, 0, W, H, 0xff15181d);
        for (int i = 0; i < NBALLS; i++) fill_circle(x[i] / FP, y[i] / FP, r[i], colors[i]);

        char line[40], *p;
        draw_text(24, 24, 5, "LEAPVM", 0xfff2f2f2);
        p = line;
        const char *label = "FRAME ";
        while (*label) *p++ = *label++;
        *fmt_num(p, frame, 7) = 0;
        draw_text(24, 80, 3, line, 0xffc8ccd2);
        uint64_t tenths = (now_ns() - start) / 100000000ULL;
        p = line;
        label = "TIME ";
        while (*label) *p++ = *label++;
        p = fmt_num(p, tenths / 10, 5);
        *p++ = '.';
        p = fmt_num(p, tenths % 10, 1);
        *p++ = ' ';
        *p++ = 'S';
        *p = 0;
        draw_text(24, 112, 3, line, 0xffc8ccd2);

        // 只把有變的列搬到螢幕：寫得越少，搬家時要追蹤的記憶體就越少
        for (int j = 0; j < H; j++) {
            const uint64_t *a = (const uint64_t *)(px + j * W), *b = (const uint64_t *)(shown + j * W);
            int same = 1;
            for (int i = 0; i < W / 2; i++)
                if (a[i] != b[i]) { same = 0; break; }
            if (same) continue;
            uint64_t *dst = (uint64_t *)(fb + j * stride), *keep = (uint64_t *)(shown + j * W);
            for (int i = 0; i < W / 2; i++) dst[i] = keep[i] = a[i];
        }

        // 每半秒把畫格數寫進 /tmp/demo.frame，讓測試程式可以確認動畫有沒有中斷
        if (frame % 30 == 0) {
            char num[16];
            char *e = fmt_num(num, frame, 7);
            *e++ = '\n';
            int out = (int)sys6(SYS_openat, -100, (long)"/tmp/demo.frame", 1 | 0x40 | 0x200 /* O_WRONLY|O_CREAT|O_TRUNC */, 0644, 0, 0);
            if (out >= 0) {
                sys6(SYS_write, out, (long)num, e - num, 0, 0, 0);
                sys6(SYS_close, out, 0, 0, 0, 0, 0);
            }
        }

        // 固定每秒 60 格
        next += 16666667;
        uint64_t t = now_ns();
        if (next > t) sleep_ns(next - t);
        else next = t;
    }
}
