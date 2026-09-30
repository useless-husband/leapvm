// LeapVM：時鐘晶片（ARM PL031 RTC）
//
// 虛擬機一開機並不知道現在幾點。沒有正確的時間，HTTPS 連線會因為「憑證還沒生效」而失敗。
// PL031 是最簡單的時鐘晶片：一個暫存器，讀出來就是「從 1970 年到現在幾秒」。
// Linux 內建它的驅動，開機時讀一次就把系統時間設好。
#include "vm.h"
#include <time.h>

enum { RTC_DR = 0x000, RTC_MR = 0x004, RTC_LR = 0x008, RTC_CR = 0x00c, RTC_IMSC = 0x010, RTC_RIS = 0x014,
       RTC_MIS = 0x018, RTC_ICR = 0x01c };

static struct {
    int64_t  offset;   // Linux 自己調過時間的話，記下跟 Mac 時間的差距
    uint32_t match, imsc;
} rtc;

static uint64_t rtc_read(void *opaque, uint64_t off, unsigned size) {
    (void)opaque;
    (void)size;
    // PrimeCell 識別碼：這是一顆 PL031
    static const uint8_t id[8] = {0x31, 0x10, 0x04, 0x00, 0x0d, 0xf0, 0x05, 0xb1};
    if (off >= 0xfe0 && off < 0x1000) return id[(off - 0xfe0) / 4];
    switch (off) {
    case RTC_DR: return (uint32_t)(time(NULL) + rtc.offset);
    case RTC_MR: return rtc.match;
    case RTC_CR: return 1;  // 一直在走
    case RTC_IMSC: return rtc.imsc;
    }
    return 0;
}

static void rtc_write(void *opaque, uint64_t off, uint64_t val, unsigned size) {
    (void)opaque;
    (void)size;
    switch (off) {
    case RTC_LR: rtc.offset = (int64_t)(uint32_t)val - time(NULL); break;
    case RTC_MR: rtc.match = (uint32_t)val; break;
    case RTC_IMSC: rtc.imsc = (uint32_t)val & 1; break;
    }
}

void rtc_init(void) {
    mmio_register(&(mmio_device_t){.name = "時鐘", .base = RTC_BASE, .size = RTC_SIZE,
                                   .read = rtc_read, .write = rtc_write});
}
