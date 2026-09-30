// LeapVM：共用定義
#pragma once
#include <Hypervisor/Hypervisor.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// ── 虛擬機的「實體」記憶體位址配置（Linux 看到的位址，稱為 IPA）──
#define GIC_DIST_BASE   0x08000000ULL  // 中斷控制器：分配器（64KB）
#define GIC_REDIST_BASE 0x0A000000ULL  // 中斷控制器：每顆 CPU 的重分配器（最多 32MB）
#define UART_BASE       0x0C000000ULL  // 序列埠 PL011
#define UART_SIZE       0x1000ULL
#define RTC_BASE        0x0C010000ULL  // 時鐘晶片 PL031
#define RTC_SIZE        0x1000ULL
#define VIRTIO_BASE     0x0D000000ULL  // virtio 裝置：每個佔 4KB，依序排下去
#define VIRTIO_SIZE     0x1000ULL
#define RAM_BASE        0x40000000ULL  // 記憶體從 1GB 位置開始

// 中斷編號（SPI）：序列埠用 1，virtio 裝置從 16 開始依序分配
#define UART_SPI        1
#define RTC_SPI         2
#define VIRTIO_SPI_BASE 16
#define VTIMER_PPI      27             // 虛擬計時器的中斷編號

// 虛擬機的螢幕：放在記憶體最後面的一塊
#define FB_WIDTH        1280
#define FB_HEIGHT       800

#define MAX_VCPUS       8
#define MiB             (1024ULL * 1024)

#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((uint64_t)(a) - 1))

#define HV(call) do { hv_return_t r_ = (call); \
    if (r_ != HV_SUCCESS) die("%s 失敗（錯誤碼 %#x）\n", #call, r_); } while (0)

#define LOG(...) fprintf(stderr, "[LeapVM] " __VA_ARGS__)

typedef struct {
    uint64_t mmio_read, mmio_write, hvc, wfi, sysreg, vtimer;
} vcpu_stats_t;

typedef struct {
    int             index;
    hv_vcpu_t       hv;
    hv_vcpu_exit_t *exit;
    pthread_t       thread;
    bool            created;
    bool            powered_on;
    bool            start_pending;  // 開機或 PSCI CPU_ON：下次執行前要設定起點
    uint64_t        start_pc, start_x0;
    bool            vtimer_masked;
    bool            offset_dirty;   // 時鐘偏移變了，下次進入虛擬機前要套用
    uint64_t        wake_seen;      // 這顆 vCPU 最後一次看到的喚醒序號
    vcpu_stats_t    stats;
} vcpu_t;

typedef struct {
    uint8_t *ram;                 // 主機端指向虛擬機記憶體的指標
    uint64_t ram_size;
    size_t   redist_region_size;
    uint32_t spi_base;            // SPI 中斷的起始 INTID
    int      ncpus;
    vcpu_t   cpus[MAX_VCPUS];
    uint32_t fb_width, fb_height; // 螢幕大小；0 表示沒有螢幕
    uint64_t fb_offset;           // 螢幕在虛擬機記憶體裡的位置（從 RAM_BASE 起算）
} vm_t;

extern vm_t vm;

// Linux 可以當一般記憶體用的大小（螢幕那一塊不算）
static inline uint64_t vm_usable_ram(void) { return vm.fb_width ? vm.fb_offset : vm.ram_size; }

// 把 Linux 看到的位址換成主機這邊的指標；超出記憶體範圍回傳 NULL
static inline void *guest_ptr(uint64_t ipa, uint64_t len) {
    if (ipa < RAM_BASE || len > vm.ram_size || ipa - RAM_BASE > vm.ram_size - len) return NULL;
    return vm.ram + (ipa - RAM_BASE);
}

// 送中斷給 Linux（SPI 編號，不是 INTID）
static inline void irq_set(uint32_t spi, bool level) { hv_gic_set_spi(vm.spi_base + spi, level); }

// ── 裝置登記表：Linux 讀寫某個位址時，交給負責那段位址的裝置處理 ──
typedef struct {
    const char *name;
    uint64_t    base, size;
    uint64_t  (*read)(void *opaque, uint64_t off, unsigned size);
    void      (*write)(void *opaque, uint64_t off, uint64_t val, unsigned size);
    void       *opaque;
} mmio_device_t;

void mmio_register(const mmio_device_t *dev);
const mmio_device_t *mmio_find(uint64_t addr);

// main.c
_Noreturn void die(const char *fmt, ...);
_Noreturn void vm_exit(int code);
void restore_terminal(void);
void print_stats(void);
void console_key(uint8_t c, bool *esc);   // 處理一個按鍵（Ctrl-A 開頭的是 LeapVM 的指令）

// cpu.c：決定虛擬機看到哪些 CPU 功能
void cpu_features_apply(hv_vcpu_t vcpu);

// fdt.c：產生硬體清單
size_t build_fdt(uint8_t *out, size_t cap, const char *bootargs,
                 uint64_t initrd_start, uint64_t initrd_end, int ncpus);

// uart.c：序列埠
void uart_init(void);
void uart_input(uint8_t c);

// rtc.c：時鐘晶片
void rtc_init(void);

// virtio_input.c：鍵盤與滑鼠（在視窗裡的操作會送到這裡）
void input_key(int linux_code, bool down);
void input_pointer_move(double x, double y);   // 0～1，左上角是 (0, 0)
void input_pointer_button(int button, bool down);  // 0 左、1 右、2 中
void input_pointer_wheel(int dy, int dx);

// vcpu.c
void vcpus_create(void);                   // 建立所有 vCPU 執行緒（停在起跑線）
void vcpu_boot(uint64_t pc, uint64_t x0);  // 設定開機 CPU 的進入點
void vm_start(void);                       // 放行：開始執行
void vm_kick_all(void);                    // 叫醒閒置中的 vCPU（有新的中斷）
void vm_clock_forward(uint64_t seconds);   // 把虛擬機的時鐘往前撥（測試用，模擬 Mac 睡了一段時間）
uint64_t host_ticks_to_ns(uint64_t ticks);

// display.m：在 Mac 上開視窗顯示虛擬機的螢幕
_Noreturn void display_run(void *(*vm_main)(void *));  // 主執行緒跑視窗，虛擬機在另一條執行緒
void display_attach(void);                // 虛擬機的螢幕準備好了，打開視窗（任何執行緒都可以呼叫）
bool display_save_png(const char *path);  // 把目前的畫面存成 PNG

// control.c：控制通道（給測試與腳本使用）
void control_start(const char *path);
int  control_send(const char *path, const char *command);
