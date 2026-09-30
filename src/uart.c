// LeapVM：模擬 ARM PL011 序列埠
//
// Linux 把文字寫到序列埠的資料暫存器時，CPU 會因為碰到「不存在的記憶體」
// 而暫停、交回給我們；我們再把那個字元印到 Mac 的終端機上。
// 反過來，Mac 鍵盤打的字放進接收佇列，再用中斷通知 Linux 來讀。
// 暫存器規格：ARM PrimeCell UART (PL011) Technical Reference Manual
#include "vm.h"
#include <string.h>
#include <unistd.h>

enum {
    DR = 0x000, RSR = 0x004, FR = 0x018, ILPR = 0x020, IBRD = 0x024, FBRD = 0x028,
    LCR_H = 0x02c, CR = 0x030, IFLS = 0x034, IMSC = 0x038, RIS = 0x03c, MIS = 0x040,
    ICR = 0x044, DMACR = 0x048,
};

// 旗標暫存器 FR
#define FR_RXFE (1u << 4)  // 接收佇列空
#define FR_RXFF (1u << 6)  // 接收佇列滿
#define FR_TXFE (1u << 7)  // 傳送佇列空（我們永遠立刻送出，所以一直是空的）

// 中斷位元
#define INT_RX (1u << 4)
#define INT_TX (1u << 5)
#define INT_RT (1u << 6)   // 接收逾時：佇列裡有資料一段時間沒被讀

#define RX_FIFO_SIZE 32

static struct {
    pthread_mutex_t lock;
    pthread_cond_t  space;  // 接收佇列有空位了
    uint8_t  rx[RX_FIFO_SIZE];
    unsigned rx_head, rx_count;
    uint32_t imsc, ibrd, fbrd, lcr_h, cr, ifls, dmacr, ilpr;
    bool     irq_level;
} u = {.lock = PTHREAD_MUTEX_INITIALIZER, .space = PTHREAD_COND_INITIALIZER, .cr = 0x300};

// 原始中斷狀態：傳送佇列永遠低於門檻，所以 TX 一直成立；有資料可讀時 RX/RT 成立
static uint32_t raw_status(void) { return INT_TX | (u.rx_count ? INT_RX | INT_RT : 0); }

// 呼叫前必須持有 u.lock
static void update_irq(void) {
    bool level = (raw_status() & u.imsc) != 0;
    if (level != u.irq_level) {
        u.irq_level = level;
        irq_set(UART_SPI, level);
    }
}

static uint64_t uart_read(void *opaque, uint64_t off, unsigned size) {
    (void)opaque;
    (void)size;
    // PrimeCell 識別碼：Linux 的 AMBA 匯流排靠這個認出「這是一顆 PL011」
    static const uint8_t id[8] = {0x11, 0x10, 0x34, 0x00, 0x0d, 0xf0, 0x05, 0xb1};
    if (off >= 0xfe0 && off < 0x1000) return id[(off - 0xfe0) / 4];

    uint32_t v = 0;
    pthread_mutex_lock(&u.lock);
    switch (off) {
    case DR:
        if (u.rx_count) {
            v = u.rx[u.rx_head];
            u.rx_head = (u.rx_head + 1) % RX_FIFO_SIZE;
            u.rx_count--;
            pthread_cond_signal(&u.space);
            update_irq();
        }
        break;
    case FR:
        v = FR_TXFE | (u.rx_count ? 0 : FR_RXFE) | (u.rx_count == RX_FIFO_SIZE ? FR_RXFF : 0);
        break;
    case ILPR:  v = u.ilpr; break;
    case IBRD:  v = u.ibrd; break;
    case FBRD:  v = u.fbrd; break;
    case LCR_H: v = u.lcr_h; break;
    case CR:    v = u.cr; break;
    case IFLS:  v = u.ifls; break;
    case IMSC:  v = u.imsc; break;
    case RIS:   v = raw_status(); break;
    case MIS:   v = raw_status() & u.imsc; break;
    case DMACR: v = u.dmacr; break;
    }
    pthread_mutex_unlock(&u.lock);
    return v;
}

static void uart_write(void *opaque, uint64_t off, uint64_t val64, unsigned size) {
    (void)opaque;
    (void)size;
    uint32_t val = (uint32_t)val64;
    if (off == DR) {
        uint8_t c = (uint8_t)val;
        (void)!write(STDOUT_FILENO, &c, 1);
        return;
    }
    pthread_mutex_lock(&u.lock);
    switch (off) {
    case ILPR:  u.ilpr = val; break;
    case IBRD:  u.ibrd = val; break;
    case FBRD:  u.fbrd = val; break;
    case LCR_H: u.lcr_h = val; break;
    case CR:    u.cr = val; break;
    case IFLS:  u.ifls = val; break;
    case IMSC:  u.imsc = val & 0x7ff; break;
    case ICR:   break;  // 我們的中斷狀態由佇列內容決定，清除動作不需要另外記錄
    case DMACR: u.dmacr = val; break;
    }
    update_irq();
    pthread_mutex_unlock(&u.lock);
}

// 主機鍵盤輸入的字元。佇列滿了就等 Linux 讀走一些（最多等 0.2 秒），
// 不然一次貼上一大段文字時後面的字會被丟掉。
void uart_input(uint8_t c) {
    pthread_mutex_lock(&u.lock);
    if (u.rx_count == RX_FIFO_SIZE) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 200 * 1000 * 1000};
        pthread_cond_timedwait_relative_np(&u.space, &u.lock, &ts);
    }
    if (u.rx_count < RX_FIFO_SIZE) {
        u.rx[(u.rx_head + u.rx_count) % RX_FIFO_SIZE] = c;
        u.rx_count++;
        update_irq();
    }
    pthread_mutex_unlock(&u.lock);
    vm_kick_all();
}

void uart_init(void) {
    mmio_register(&(mmio_device_t){.name = "序列埠", .base = UART_BASE, .size = UART_SIZE,
                                   .read = uart_read, .write = uart_write});
    pthread_mutex_lock(&u.lock);
    update_irq();
    pthread_mutex_unlock(&u.lock);
}
