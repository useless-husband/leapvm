// LeapVM：virtio 裝置的共用部分
//
// virtio 是專門給虛擬機用的「假硬體」標準，Linux 內建支援。
// Linux 和 LeapVM 在虛擬機記憶體裡共用幾條「佇列」（virtqueue）：
//   1. Linux 把要做的事（例如「讀硬碟第 100 格到這塊記憶體」）寫成描述，放進 available 環
//   2. Linux 寫一下 QueueNotify 暫存器（敲門鈴）→ 這是唯一需要離開虛擬機的時候
//   3. LeapVM 做完，把結果放進 used 環，再送一個中斷通知 Linux
// 一次敲門可以處理一整批，所以 VM exit 很少，效能損失很小。
// 規格：Virtual I/O Device (VIRTIO) Version 1.2，第 2 章（佇列）與第 4.2 節（MMIO 傳輸）
#pragma once
#include "vm.h"
#include <sys/uio.h>

#define VIRTQ_MAX_SIZE 1024
#define VIRTIO_MAX_QUEUES 4

// 佇列在記憶體裡的格式（規格第 2.7 節）
struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};
#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2  // 這塊是給裝置寫的（例如讀硬碟時放資料的地方）

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
};
#define VRING_AVAIL_F_NO_INTERRUPT 1

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
};

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[];
};
#define VRING_USED_F_NO_NOTIFY 1

#define VIRTIO_F_VERSION_1 (1ULL << 32)

typedef struct {
    uint32_t num;                 // 佇列長度
    bool     ready;
    uint64_t desc_addr, avail_addr, used_addr;
    struct vring_desc  *desc;     // 換算成主機指標後的位置
    struct vring_avail *avail;
    struct vring_used  *used;
    uint16_t last_avail;          // 下一個要處理的項目
} virtq_t;

// 一段緩衝區（已經換算成主機指標）
typedef struct {
    void    *ptr;
    uint32_t len;
    bool     write;               // true：裝置寫給 Linux；false：Linux 給裝置讀
} vbuf_t;

typedef struct virtio_dev virtio_dev_t;

struct virtio_dev {
    const char *name;
    uint32_t    device_id;        // 1 = 網路卡、2 = 硬碟、18 = 輸入裝置
    uint64_t    features;         // 裝置支援的功能
    uint64_t    driver_features;  // Linux 決定要用的功能
    int         nqueues;
    uint32_t    queue_max;

    // 裝置自己的部分
    void     (*notify)(virtio_dev_t *d, int queue);   // Linux 敲了門鈴（在 vCPU 執行緒上，要快）
    uint64_t (*config_read)(virtio_dev_t *d, uint64_t off, unsigned size);
    void     (*config_write)(virtio_dev_t *d, uint64_t off, uint64_t val, unsigned size);
    void     (*reset)(virtio_dev_t *d);
    void     (*print_stats)(virtio_dev_t *d);   // 印出裝置自己的統計（可以不提供）
    void      *priv;

    // 以下由 virtio.c 管理
    pthread_mutex_t lock;
    uint64_t    base;
    uint32_t    spi;
    uint32_t    status;
    uint32_t    dev_feat_sel, drv_feat_sel, queue_sel;
    uint32_t    isr;
    virtq_t     vq[VIRTIO_MAX_QUEUES];
    uint64_t    stat_notify, stat_irq;  // 敲門鈴次數、送中斷次數
};

void virtio_register(virtio_dev_t *d);   // 分配位址與中斷編號、登記到裝置表
int  virtio_count(void);
virtio_dev_t *virtio_get(int i);
void virtio_print_stats(void);           // 所有裝置的統計：敲門鈴幾次、送中斷幾次、各裝置自己的數字

// 取出下一個待辦項目（一串緩衝區）。沒有就回傳 -1。
// 回傳值是這個項目的編號，做完要交給 virtq_push。
int  virtq_pop(virtio_dev_t *d, virtq_t *q, vbuf_t *bufs, int max_bufs, int *nbufs);
void virtq_push(virtio_dev_t *d, virtq_t *q, int head, uint32_t written);
bool virtq_has_work(virtio_dev_t *d, virtq_t *q);

// 處理佇列期間暫停門鈴（Linux 放新項目時不用敲門），處理完再打開。
// 打開後會再檢查一次，回傳 true 表示剛好又有新項目，要繼續處理。
void virtq_disable_notify(virtq_t *q);
bool virtq_enable_notify(virtq_t *q);

// 通知 Linux「有做完的項目」。Linux 表示不需要時會自動略過。
void virtio_interrupt(virtio_dev_t *d, virtq_t *q);

// 把一串緩衝區當成連續的資料來讀寫
size_t vbuf_copy_from(const vbuf_t *bufs, int n, size_t skip, void *dst, size_t len);
size_t vbuf_copy_to(const vbuf_t *bufs, int n, size_t skip, const void *src, size_t len);

// 裝置
void virtio_blk_init(const char *path);
void virtio_net_init(void);
void virtio_input_init(void);
