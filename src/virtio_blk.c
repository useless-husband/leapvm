// LeapVM：虛擬硬碟（virtio-blk）
//
// 硬碟的內容存在 Mac 上的一個檔案裡（例如 disk.img）。Linux 要讀寫硬碟時，
// 我們直接用 preadv / pwritev 把檔案內容搬進搬出「Linux 指定的那塊記憶體」，
// 中間不另外複製一份，所以幾乎沒有額外成本。
// 規格：VIRTIO 1.2 第 5.2 節
#include "virtio.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { BLK_T_IN = 0, BLK_T_OUT = 1, BLK_T_FLUSH = 4, BLK_T_GET_ID = 8 };
enum { BLK_S_OK = 0, BLK_S_IOERR = 1, BLK_S_UNSUPP = 2 };
#define VIRTIO_BLK_F_SEG_MAX (1ULL << 2)
#define VIRTIO_BLK_F_FLUSH   (1ULL << 9)
#define SECTOR 512
#define MAX_SEGS 128

struct blk_req_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

static struct {
    virtio_dev_t    dev;
    int             fd;
    uint64_t        sectors;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    bool            pending;  // Linux 敲過門鈴、還沒處理
    uint64_t        reads, writes, bytes_read, bytes_written;
} blk = {.lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER};

static uint8_t handle_request(vbuf_t *bufs, int n, uint32_t *written) {
    struct blk_req_hdr h;
    // 第一段是請求的標頭，最後一段（1 byte）是給我們回報結果的
    if (n < 2 || vbuf_copy_from(bufs, n, 0, &h, sizeof h) != sizeof h) return BLK_S_IOERR;
    if (!bufs[n - 1].write || bufs[n - 1].len < 1) return BLK_S_IOERR;

    // 中間那幾段是資料：標頭之後、狀態之前
    struct iovec iov[MAX_SEGS];
    int niov = 0;
    size_t total = 0, skip = sizeof h;
    for (int i = 0; i < n; i++) {
        size_t len = bufs[i].len;
        if (i == n - 1) len -= 1;  // 最後一段的最後 1 byte 是狀態
        if (skip >= len) { skip -= len; continue; }
        if (niov == MAX_SEGS) return BLK_S_IOERR;
        iov[niov++] = (struct iovec){(uint8_t *)bufs[i].ptr + skip, len - skip};
        total += len - skip;
        skip = 0;
    }

    // 範圍檢查要小心整數溢位：先確認 sector 本身沒超過，再確認長度放得下
    bool in_range = h.sector <= blk.sectors && total % SECTOR == 0 && total / SECTOR <= blk.sectors - h.sector;
    off_t pos = (off_t)(h.sector * SECTOR);
    switch (h.type) {
    case BLK_T_IN:
        if (!in_range) return BLK_S_IOERR;
        if (preadv(blk.fd, iov, niov, pos) != (ssize_t)total) return BLK_S_IOERR;
        *written = (uint32_t)total;
        blk.reads++;
        blk.bytes_read += total;
        return BLK_S_OK;
    case BLK_T_OUT:
        if (!in_range) return BLK_S_IOERR;
        if (pwritev(blk.fd, iov, niov, pos) != (ssize_t)total) return BLK_S_IOERR;
        blk.writes++;
        blk.bytes_written += total;
        return BLK_S_OK;
    case BLK_T_FLUSH:
        // F_BARRIERFSYNC：確保之前寫的都先落地，比 F_FULLFSYNC 快很多
        return fcntl(blk.fd, F_BARRIERFSYNC) == 0 || fsync(blk.fd) == 0 ? BLK_S_OK : BLK_S_IOERR;
    case BLK_T_GET_ID: {
        static const char id[20] = "leapvm-disk";
        size_t k = total < sizeof id ? total : sizeof id;
        if (niov) memcpy(iov[0].iov_base, id, k < iov[0].iov_len ? k : iov[0].iov_len);
        *written = (uint32_t)k;
        return BLK_S_OK;
    }
    }
    return BLK_S_UNSUPP;
}

static void *blk_thread(void *arg) {
    (void)arg;
    virtio_dev_t *d = &blk.dev;
    virtq_t *q = &d->vq[0];
    vbuf_t bufs[MAX_SEGS + 2];
    for (;;) {
        pthread_mutex_lock(&blk.lock);
        while (!blk.pending) pthread_cond_wait(&blk.cond, &blk.lock);
        blk.pending = false;
        pthread_mutex_unlock(&blk.lock);
        if (!q->ready) continue;

        // 一次把佇列裡的請求全部做完，最後才送一次中斷
        bool did = false;
        do {
            virtq_disable_notify(q);
            int n, head;
            while ((head = virtq_pop(d, q, bufs, MAX_SEGS + 2, &n)) >= 0) {
                uint32_t written = 0;
                uint8_t status = handle_request(bufs, n, &written);
                *((uint8_t *)bufs[n - 1].ptr + bufs[n - 1].len - 1) = status;
                virtq_push(d, q, head, written + 1);
                did = true;
            }
        } while (virtq_enable_notify(q));
        if (did) virtio_interrupt(d, q);
    }
    return NULL;
}

static void blk_notify(virtio_dev_t *d, int queue) {
    (void)d;
    (void)queue;
    pthread_mutex_lock(&blk.lock);
    blk.pending = true;
    pthread_cond_signal(&blk.cond);
    pthread_mutex_unlock(&blk.lock);
}

static void blk_print_stats(virtio_dev_t *d) {
    (void)d;
    fprintf(stderr, "｜讀 %llu 次 %.1f MB｜寫 %llu 次 %.1f MB", blk.reads, blk.bytes_read / 1048576.0, blk.writes,
            blk.bytes_written / 1048576.0);
}

// 設定空間：容量（以 512 bytes 為單位）與每個請求最多幾段
static uint64_t blk_config_read(virtio_dev_t *d, uint64_t off, unsigned size) {
    (void)d;
    uint8_t cfg[16] = {0};
    memcpy(cfg, &blk.sectors, 8);
    uint32_t seg_max = MAX_SEGS - 2;
    memcpy(cfg + 12, &seg_max, 4);
    uint64_t v = 0;
    if (off + size <= sizeof cfg) memcpy(&v, cfg + off, size);
    return v;
}

void virtio_blk_init(const char *path) {
    blk.fd = open(path, O_RDWR);
    if (blk.fd < 0) die("打不開硬碟檔 %s：%s\n", path, strerror(errno));
    struct stat sb;
    fstat(blk.fd, &sb);
    blk.sectors = (uint64_t)sb.st_size / SECTOR;
    blk.dev = (virtio_dev_t){
        .name = "硬碟",
        .device_id = 2,
        .features = VIRTIO_BLK_F_SEG_MAX | VIRTIO_BLK_F_FLUSH,
        .nqueues = 1,
        .queue_max = 256,
        .notify = blk_notify,
        .config_read = blk_config_read,
        .print_stats = blk_print_stats,
    };
    virtio_register(&blk.dev);
    pthread_t t;
    pthread_create(&t, NULL, blk_thread, NULL);
    pthread_detach(t);
    LOG("硬碟：%s（%.1f GB）\n", path, (double)sb.st_size / (1ULL << 30));
}
