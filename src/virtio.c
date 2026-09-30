// LeapVM：virtio-mmio 傳輸層與佇列處理（所有 virtio 裝置共用）
#include "virtio.h"
#include <string.h>

// virtio-mmio 暫存器（規格第 4.2.2 節）
enum {
    REG_MAGIC = 0x000, REG_VERSION = 0x004, REG_DEVICE_ID = 0x008, REG_VENDOR_ID = 0x00c,
    REG_DEVICE_FEATURES = 0x010, REG_DEVICE_FEATURES_SEL = 0x014,
    REG_DRIVER_FEATURES = 0x020, REG_DRIVER_FEATURES_SEL = 0x024,
    REG_QUEUE_SEL = 0x030, REG_QUEUE_NUM_MAX = 0x034, REG_QUEUE_NUM = 0x038, REG_QUEUE_READY = 0x044,
    REG_QUEUE_NOTIFY = 0x050, REG_INTERRUPT_STATUS = 0x060, REG_INTERRUPT_ACK = 0x064, REG_STATUS = 0x070,
    REG_QUEUE_DESC_LOW = 0x080, REG_QUEUE_DESC_HIGH = 0x084,
    REG_QUEUE_AVAIL_LOW = 0x090, REG_QUEUE_AVAIL_HIGH = 0x094,
    REG_QUEUE_USED_LOW = 0x0a0, REG_QUEUE_USED_HIGH = 0x0a4,
    REG_CONFIG_GENERATION = 0x0fc, REG_CONFIG = 0x100,
};

#define MAX_VIRTIO 8
static virtio_dev_t *devs[MAX_VIRTIO];
static int ndevs;

int virtio_count(void) { return ndevs; }
virtio_dev_t *virtio_get(int i) { return devs[i]; }

void virtio_print_stats(void) {
    for (int i = 0; i < ndevs; i++) {
        virtio_dev_t *d = devs[i];
        LOG("%s：門鈴 %llu 次｜中斷 %llu 次", d->name, d->stat_notify, d->stat_irq);
        if (d->print_stats) d->print_stats(d);
        fputs("\r\n", stderr);
    }
}

// 呼叫前必須持有 d->lock
static void update_irq(virtio_dev_t *d) { irq_set(d->spi, d->isr != 0); }

// Linux 重設裝置（關機、重新載入驅動時）。裝置執行緒可能正在用佇列，
// 所以只把 ready 關掉、清掉位址，不把指標和長度歸零：舊指標仍指向虛擬機記憶體，
// 執行緒最多讀到過期的內容，不會碰到 NULL 或除以零。下次 QUEUE_READY 時會重新對應。
static void reset(virtio_dev_t *d) {
    d->status = 0;
    d->driver_features = 0;
    d->isr = 0;
    d->queue_sel = d->dev_feat_sel = d->drv_feat_sel = 0;
    for (int i = 0; i < VIRTIO_MAX_QUEUES; i++) {
        virtq_t *q = &d->vq[i];
        __atomic_store_n(&q->ready, false, __ATOMIC_SEQ_CST);
        q->desc_addr = q->avail_addr = q->used_addr = 0;
        q->last_avail = 0;
    }
    update_irq(d);
    if (d->reset) d->reset(d);
}

static void set64(uint64_t *field, bool high, uint32_t v) {
    *field = high ? (*field & 0xffffffffULL) | ((uint64_t)v << 32) : (*field & ~0xffffffffULL) | v;
}

// Linux 把佇列設成 ready 時，把它給的位址換成主機指標，之後直接讀寫
static bool map_queue(virtq_t *q) {
    if (!q->num || q->num > VIRTQ_MAX_SIZE || (q->num & (q->num - 1))) return false;
    q->desc = guest_ptr(q->desc_addr, 16ULL * q->num);
    q->avail = guest_ptr(q->avail_addr, 6 + 2ULL * q->num);
    q->used = guest_ptr(q->used_addr, 6 + 8ULL * q->num);
    return q->desc && q->avail && q->used;
}

static uint64_t mmio_read(void *opaque, uint64_t off, unsigned size) {
    virtio_dev_t *d = opaque;
    if (off >= REG_CONFIG) return d->config_read ? d->config_read(d, off - REG_CONFIG, size) : 0;
    pthread_mutex_lock(&d->lock);
    bool valid_q = (int)d->queue_sel < d->nqueues && d->queue_sel < VIRTIO_MAX_QUEUES;
    virtq_t *q = &d->vq[valid_q ? d->queue_sel : 0];
    uint64_t v = 0;
    switch (off) {
    case REG_MAGIC: v = 0x74726976; break;  // "virt"
    case REG_VERSION: v = 2; break;
    case REG_DEVICE_ID: v = d->device_id; break;
    case REG_VENDOR_ID: v = 0x4c454150; break;  // "LEAP"
    case REG_DEVICE_FEATURES: v = d->dev_feat_sel < 2 ? (uint32_t)(d->features >> (32 * d->dev_feat_sel)) : 0; break;
    case REG_QUEUE_NUM_MAX: v = valid_q ? d->queue_max : 0; break;
    case REG_QUEUE_READY: v = valid_q && q->ready; break;
    case REG_INTERRUPT_STATUS: v = d->isr; break;
    case REG_STATUS: v = d->status; break;
    case REG_CONFIG_GENERATION: v = 0; break;
    }
    pthread_mutex_unlock(&d->lock);
    return v;
}

static void mmio_write(void *opaque, uint64_t off, uint64_t val64, unsigned size) {
    virtio_dev_t *d = opaque;
    if (off >= REG_CONFIG) {
        if (d->config_write) d->config_write(d, off - REG_CONFIG, val64, size);
        return;
    }
    uint32_t val = (uint32_t)val64;
    if (off == REG_QUEUE_NOTIFY) {  // 最常發生、最要緊的一個：不拿鎖，直接交給裝置
        d->stat_notify++;
        if ((int)val < d->nqueues && d->notify) d->notify(d, (int)val);
        return;
    }
    pthread_mutex_lock(&d->lock);
    // Linux 選了不存在的佇列時，佇列相關的寫入一律忽略（不能誤改到第 0 號）
    bool valid_q = (int)d->queue_sel < d->nqueues && d->queue_sel < VIRTIO_MAX_QUEUES;
    virtq_t *q = valid_q ? &d->vq[d->queue_sel] : NULL;
    if (!q && off >= REG_QUEUE_NUM && off <= REG_QUEUE_USED_HIGH && off != REG_QUEUE_NOTIFY &&
        off != REG_INTERRUPT_STATUS && off != REG_INTERRUPT_ACK && off != REG_STATUS) {
        pthread_mutex_unlock(&d->lock);
        return;
    }
    switch (off) {
    case REG_DEVICE_FEATURES_SEL: d->dev_feat_sel = val; break;
    case REG_DRIVER_FEATURES_SEL: d->drv_feat_sel = val; break;
    case REG_DRIVER_FEATURES:
        if (d->drv_feat_sel < 2) set64(&d->driver_features, d->drv_feat_sel == 1, val);
        d->driver_features &= d->features;  // 只能用裝置有支援的功能
        break;
    case REG_QUEUE_SEL: d->queue_sel = val; break;
    case REG_QUEUE_NUM: q->num = val; break;
    case REG_QUEUE_DESC_LOW: set64(&q->desc_addr, false, val); break;
    case REG_QUEUE_DESC_HIGH: set64(&q->desc_addr, true, val); break;
    case REG_QUEUE_AVAIL_LOW: set64(&q->avail_addr, false, val); break;
    case REG_QUEUE_AVAIL_HIGH: set64(&q->avail_addr, true, val); break;
    case REG_QUEUE_USED_LOW: set64(&q->used_addr, false, val); break;
    case REG_QUEUE_USED_HIGH: set64(&q->used_addr, true, val); break;
    case REG_QUEUE_READY:
        if (val && !map_queue(q)) {
            LOG("%s：佇列 %u 的位址不合理，不啟用\r\n", d->name, d->queue_sel);
            break;
        }
        q->last_avail = 0;
        __atomic_store_n(&q->ready, (bool)(val & 1), __ATOMIC_SEQ_CST);
        break;
    case REG_INTERRUPT_ACK:
        d->isr &= ~val;
        update_irq(d);
        break;
    case REG_STATUS:
        if (val == 0) reset(d);
        else d->status = val;
        break;
    }
    pthread_mutex_unlock(&d->lock);
}

void virtio_register(virtio_dev_t *d) {
    if (ndevs == MAX_VIRTIO) die("virtio 裝置太多\n");
    d->base = VIRTIO_BASE + (uint64_t)ndevs * VIRTIO_SIZE;
    d->spi = VIRTIO_SPI_BASE + (uint32_t)ndevs;
    d->features |= VIRTIO_F_VERSION_1;
    if (!d->queue_max) d->queue_max = VIRTQ_MAX_SIZE;
    pthread_mutex_init(&d->lock, NULL);
    devs[ndevs++] = d;
    mmio_register(&(mmio_device_t){.name = d->name, .base = d->base, .size = VIRTIO_SIZE,
                                   .read = mmio_read, .write = mmio_write, .opaque = d});
}

// ── 佇列 ──
// 共用記憶體的讀寫順序很重要：先讀 idx 再讀內容；先寫內容再寫 idx。
// 用 acquire / release 確保 CPU 不會把順序調換。

bool virtq_has_work(virtio_dev_t *d, virtq_t *q) {
    (void)d;
    return __atomic_load_n(&q->ready, __ATOMIC_ACQUIRE) &&
           __atomic_load_n(&q->avail->idx, __ATOMIC_ACQUIRE) != q->last_avail;
}

int virtq_pop(virtio_dev_t *d, virtq_t *q, vbuf_t *bufs, int max_bufs, int *nbufs) {
    if (!virtq_has_work(d, q)) return -1;
    uint16_t head = q->avail->ring[q->last_avail % q->num];
    q->last_avail++;
    if (head >= q->num) die("%s：佇列項目編號不合理\n", d->name);

    int n = 0;
    uint16_t i = head;
    for (unsigned hops = 0;; hops++) {
        if (hops >= q->num || n == max_bufs) die("%s：描述串太長或形成迴圈\n", d->name);
        // 描述子在 Linux 也能改的共用記憶體裡：先整個抄一份，之後只看副本，
        // 免得檢查完長度之後它又被改掉
        struct vring_desc desc;
        memcpy(&desc, &q->desc[i], sizeof desc);
        void *p = guest_ptr(desc.addr, desc.len);
        if (!p) die("%s：緩衝區位址超出記憶體範圍\n", d->name);
        bufs[n++] = (vbuf_t){p, desc.len, desc.flags & VRING_DESC_F_WRITE};
        if (!(desc.flags & VRING_DESC_F_NEXT)) break;
        i = desc.next;
        if (i >= q->num) die("%s：描述串的下一項編號不合理\n", d->name);
    }
    *nbufs = n;
    return head;
}

void virtq_push(virtio_dev_t *d, virtq_t *q, int head, uint32_t written) {
    (void)d;
    uint16_t idx = q->used->idx;
    q->used->ring[idx % q->num] = (struct vring_used_elem){(uint32_t)head, written};
    __atomic_store_n(&q->used->idx, (uint16_t)(idx + 1), __ATOMIC_RELEASE);
}

void virtq_disable_notify(virtq_t *q) {
    __atomic_or_fetch(&q->used->flags, VRING_USED_F_NO_NOTIFY, __ATOMIC_RELAXED);
}

bool virtq_enable_notify(virtq_t *q) {
    __atomic_and_fetch(&q->used->flags, (uint16_t)~VRING_USED_F_NO_NOTIFY, __ATOMIC_SEQ_CST);
    // 打開門鈴之後再看一次：Linux 可能在門鈴關著的期間放了新項目，而且沒有敲門
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return __atomic_load_n(&q->avail->idx, __ATOMIC_ACQUIRE) != q->last_avail;
}

void virtio_interrupt(virtio_dev_t *d, virtq_t *q) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&q->avail->flags, __ATOMIC_RELAXED) & VRING_AVAIL_F_NO_INTERRUPT) return;
    pthread_mutex_lock(&d->lock);
    d->isr |= 1;
    d->stat_irq++;
    update_irq(d);
    pthread_mutex_unlock(&d->lock);
    vm_kick_all();
}

size_t vbuf_copy_from(const vbuf_t *bufs, int n, size_t skip, void *dst, size_t len) {
    size_t done = 0;
    for (int i = 0; i < n && done < len; i++) {
        if (skip >= bufs[i].len) { skip -= bufs[i].len; continue; }
        size_t k = bufs[i].len - skip;
        if (k > len - done) k = len - done;
        memcpy((uint8_t *)dst + done, (uint8_t *)bufs[i].ptr + skip, k);
        done += k;
        skip = 0;
    }
    return done;
}

size_t vbuf_copy_to(const vbuf_t *bufs, int n, size_t skip, const void *src, size_t len) {
    size_t done = 0;
    for (int i = 0; i < n && done < len; i++) {
        if (!bufs[i].write) continue;
        if (skip >= bufs[i].len) { skip -= bufs[i].len; continue; }
        size_t k = bufs[i].len - skip;
        if (k > len - done) k = len - done;
        memcpy((uint8_t *)bufs[i].ptr + skip, (const uint8_t *)src + done, k);
        done += k;
        skip = 0;
    }
    return done;
}
