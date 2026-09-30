// LeapVM：產生 Device Tree（硬體清單）
//
// Linux 開機時不會自己去「找」硬體，而是讀一份二進位格式的清單（FDT），
// 裡面寫著記憶體在哪、有幾顆 CPU、中斷控制器和序列埠的位址。
// 格式規格：https://devicetree-specification.readthedocs.io
#include "virtio.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define FDT_MAGIC      0xd00dfeed
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE   2
#define FDT_PROP       3
#define FDT_END        9

#define PHANDLE_GIC 1
#define PHANDLE_CLK 2

static uint8_t st[16384];   // structure block：節點與屬性
static size_t  st_len;
static char    strs[2048];  // strings block：屬性名稱（重複的名稱只存一次）
static size_t  strs_len;

static void put32(uint32_t v) {
    if (st_len + 4 > sizeof st) die("FDT 太大\n");
    v = __builtin_bswap32(v);  // FDT 一律是大端序
    memcpy(st + st_len, &v, 4);
    st_len += 4;
}

static void put_bytes(const void *p, size_t n) {
    if (st_len + n + 4 > sizeof st) die("FDT 太大\n");
    if (n) memcpy(st + st_len, p, n);
    st_len += n;
    while (st_len & 3) st[st_len++] = 0;  // 對齊到 4 bytes
}

static uint32_t str_off(const char *s) {
    for (size_t i = 0; i < strs_len; i += strlen(strs + i) + 1)
        if (!strcmp(strs + i, s)) return (uint32_t)i;
    size_t n = strlen(s) + 1;
    if (strs_len + n > sizeof strs) die("FDT 字串區太大\n");
    memcpy(strs + strs_len, s, n);
    uint32_t off = (uint32_t)strs_len;
    strs_len += n;
    return off;
}

static void begin(const char *name) { put32(FDT_BEGIN_NODE); put_bytes(name, strlen(name) + 1); }
static void end(void) { put32(FDT_END_NODE); }

static void prop(const char *name, const void *data, size_t len) {
    put32(FDT_PROP);
    put32((uint32_t)len);
    put32(str_off(name));
    put_bytes(data, len);
}

static void prop_str(const char *name, const char *s) { prop(name, s, strlen(s) + 1); }
static void prop_empty(const char *name) { prop(name, NULL, 0); }

// 字串清單，例如 compatible = "arm,pl011", "arm,primecell"（以 \0 分隔）
static void prop_strlist(const char *name, const char *list, size_t len) { prop(name, list, len); }

static void prop_cells(const char *name, int n, ...) {
    uint32_t c[32];
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) c[i] = __builtin_bswap32(va_arg(ap, uint32_t));
    va_end(ap);
    prop(name, c, (size_t)n * 4);
}

static void prop_u32(const char *name, uint32_t v) { prop_cells(name, 1, v); }
static void prop_u64(const char *name, uint64_t v) { prop_cells(name, 2, (uint32_t)(v >> 32), (uint32_t)v); }

// reg = <位址 大小>，位址與大小各用兩個 32-bit cell 表示
static void prop_reg(uint64_t addr, uint64_t size) {
    prop_cells("reg", 4, (uint32_t)(addr >> 32), (uint32_t)addr, (uint32_t)(size >> 32), (uint32_t)size);
}

size_t build_fdt(uint8_t *out, size_t cap, const char *bootargs,
                 uint64_t initrd_start, uint64_t initrd_end, int ncpus) {
    char name[64];
    st_len = strs_len = 0;

    begin("");  // 根節點
    prop_str("compatible", "linux,dummy-virt");
    prop_str("model", "LeapVM");
    prop_u32("#address-cells", 2);
    prop_u32("#size-cells", 2);
    prop_u32("interrupt-parent", PHANDLE_GIC);

    begin("chosen");
    prop_str("bootargs", bootargs);
    snprintf(name, sizeof name, "/pl011@%llx", UART_BASE);
    prop_str("stdout-path", name);
    if (initrd_end > initrd_start) {
        prop_u64("linux,initrd-start", initrd_start);
        prop_u64("linux,initrd-end", initrd_end);
    }
    end();

    snprintf(name, sizeof name, "memory@%llx", RAM_BASE);
    begin(name);
    prop_str("device_type", "memory");
    prop_reg(RAM_BASE, vm_usable_ram());  // 螢幕那一塊不能給 Linux 當一般記憶體用
    end();

    if (vm.fb_width) {
        snprintf(name, sizeof name, "framebuffer@%llx", RAM_BASE + vm.fb_offset);
        begin(name);
        prop_str("compatible", "simple-framebuffer");
        prop_reg(RAM_BASE + vm.fb_offset, (uint64_t)vm.fb_width * vm.fb_height * 4);
        prop_u32("width", vm.fb_width);
        prop_u32("height", vm.fb_height);
        prop_u32("stride", vm.fb_width * 4);
        prop_str("format", "a8r8g8b8");  // 每個像素 4 bytes，記憶體裡的順序是藍、綠、紅、透明度
        end();
    }

    begin("cpus");
    prop_u32("#address-cells", 1);
    prop_u32("#size-cells", 0);
    for (int i = 0; i < ncpus; i++) {
        snprintf(name, sizeof name, "cpu@%d", i);
        begin(name);
        prop_str("device_type", "cpu");
        prop_str("compatible", "arm,arm-v8");
        prop_u32("reg", (uint32_t)i);  // 必須跟這顆 vCPU 的 MPIDR_EL1 一致
        prop_str("enable-method", "psci");
        end();
    }
    end();

    // PSCI：Linux 用 HVC 指令呼叫我們來開關 CPU、關機
    begin("psci");
    static const char psci_compat[] = "arm,psci-1.0\0arm,psci-0.2";
    prop_strlist("compatible", psci_compat, sizeof psci_compat);
    prop_str("method", "hvc");
    end();

    // GICv3 中斷控制器：實際由 Apple 的 Hypervisor.framework 模擬
    snprintf(name, sizeof name, "intc@%llx", GIC_DIST_BASE);
    begin(name);
    prop_str("compatible", "arm,gic-v3");
    prop_u32("#interrupt-cells", 3);
    prop_empty("interrupt-controller");
    prop_cells("reg", 8,
               (uint32_t)(GIC_DIST_BASE >> 32), (uint32_t)GIC_DIST_BASE, 0u, 0x10000u,
               (uint32_t)(GIC_REDIST_BASE >> 32), (uint32_t)GIC_REDIST_BASE,
               (uint32_t)((uint64_t)vm.redist_region_size >> 32), (uint32_t)vm.redist_region_size);
    prop_u32("phandle", PHANDLE_GIC);
    end();

    // ARM 通用計時器：<類型 編號 旗標>，類型 1 = PPI，旗標 4 = 高準位觸發
    // 順序：安全實體、非安全實體、虛擬、Hypervisor 實體
    begin("timer");
    prop_str("compatible", "arm,armv8-timer");
    prop_cells("interrupts", 12, 1u, 13u, 4u, 1u, 14u, 4u, 1u, 11u, 4u, 1u, 10u, 4u);
    end();

    begin("apb-pclk");
    prop_str("compatible", "fixed-clock");
    prop_u32("#clock-cells", 0);
    prop_u32("clock-frequency", 24000000);
    prop_str("clock-output-names", "clk24mhz");
    prop_u32("phandle", PHANDLE_CLK);
    end();

    snprintf(name, sizeof name, "pl011@%llx", UART_BASE);
    begin(name);
    static const char uart_compat[] = "arm,pl011\0arm,primecell";
    prop_strlist("compatible", uart_compat, sizeof uart_compat);
    prop_reg(UART_BASE, UART_SIZE);
    prop_cells("interrupts", 3, 0u, (uint32_t)UART_SPI, 4u);  // 類型 0 = SPI
    prop_cells("clocks", 2, (uint32_t)PHANDLE_CLK, (uint32_t)PHANDLE_CLK);
    static const char clk_names[] = "uartclk\0apb_pclk";
    prop_strlist("clock-names", clk_names, sizeof clk_names);
    end();

    snprintf(name, sizeof name, "rtc@%llx", RTC_BASE);
    begin(name);
    static const char rtc_compat[] = "arm,pl031\0arm,primecell";
    prop_strlist("compatible", rtc_compat, sizeof rtc_compat);
    prop_reg(RTC_BASE, RTC_SIZE);
    prop_cells("interrupts", 3, 0u, (uint32_t)RTC_SPI, 4u);
    prop_u32("clocks", PHANDLE_CLK);
    prop_str("clock-names", "apb_pclk");
    end();

    // virtio 裝置：dma-coherent 告訴 Linux「裝置跟 CPU 看到的記憶體內容一致」，
    // 不用額外清快取，也不用繞道複製，這對效能很重要
    for (int i = 0; i < virtio_count(); i++) {
        virtio_dev_t *d = virtio_get(i);
        snprintf(name, sizeof name, "virtio_mmio@%llx", d->base);
        begin(name);
        prop_str("compatible", "virtio,mmio");
        prop_reg(d->base, 0x200);
        prop_cells("interrupts", 3, 0u, d->spi, 4u);
        prop_empty("dma-coherent");
        end();
    }

    end();  // 根節點結束
    put32(FDT_END);

    // 組合：header(40) + 保留記憶體表(16，空的) + structure + strings
    const uint32_t off_rsv = 40, off_st = off_rsv + 16;
    const uint32_t off_str = off_st + (uint32_t)st_len;
    const uint32_t total = off_str + (uint32_t)strs_len;
    if (total > cap) die("FDT 超出可用空間\n");

    uint32_t hdr[10] = {FDT_MAGIC, total, off_st, off_str, off_rsv, 17, 16, 0,
                        (uint32_t)strs_len, (uint32_t)st_len};
    for (int i = 0; i < 10; i++) hdr[i] = __builtin_bswap32(hdr[i]);
    memset(out, 0, off_st);
    memcpy(out, hdr, sizeof hdr);
    memcpy(out + off_st, st, st_len);
    memcpy(out + off_str, strs, strs_len);
    return total;
}
