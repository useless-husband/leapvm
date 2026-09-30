// LeapVM：虛擬鍵盤與滑鼠（virtio-input）
//
// 你在 LeapVM 視窗裡按鍵、移動滑鼠，Mac 會通知 display.m；display.m 把它換算成
// Linux 的「輸入事件」（跟真的 USB 鍵盤滑鼠送給 Linux 的格式一樣），放進這裡的佇列。
//
// 滑鼠用「絕對座標」（像觸控板上的一個點，而不是移動了多少）：
// Mac 的游標在視窗哪裡，Linux 的游標就在畫面的哪裡，不用把游標鎖在視窗裡。
// 規格：VIRTIO 1.2 第 5.8 節；事件編號來自 Linux 的 input-event-codes.h
#include "virtio.h"
#include <stdlib.h>
#include <string.h>

enum { EV_SYN = 0, EV_KEY = 1, EV_REL = 2, EV_ABS = 3, EV_REP = 0x14 };
enum { SYN_REPORT = 0, REL_HWHEEL = 6, REL_WHEEL = 8, ABS_X = 0, ABS_Y = 1 };
enum { BTN_LEFT = 0x110, BTN_RIGHT = 0x111, BTN_MIDDLE = 0x112 };
enum { CFG_UNSET = 0, CFG_ID_NAME = 1, CFG_ID_SERIAL = 2, CFG_ID_DEVIDS = 3, CFG_PROP_BITS = 0x10,
       CFG_EV_BITS = 0x11, CFG_ABS_INFO = 0x12 };
#define ABS_MAX_VALUE 32767
#define EVENTQ 0
#define STATUSQ 1
#define PENDING_MAX 512

struct input_event {
    uint16_t type;
    uint16_t code;
    uint32_t value;
};

typedef struct {
    virtio_dev_t dev;
    const char  *name;
    bool         pointer;         // false = 鍵盤、true = 滑鼠
    uint8_t      select, subsel;
    pthread_mutex_t lock;
    struct input_event pending[PENDING_MAX];
    int          npending;
} vinput_t;

static vinput_t kbd = {.name = "LeapVM Keyboard", .pointer = false, .lock = PTHREAD_MUTEX_INITIALIZER};
static vinput_t mouse = {.name = "LeapVM Tablet", .pointer = true, .lock = PTHREAD_MUTEX_INITIALIZER};

static vinput_t *of(virtio_dev_t *d) { return d == &kbd.dev ? &kbd : &mouse; }

static void set_bit(uint8_t *bits, unsigned n) { bits[n / 8] |= (uint8_t)(1u << (n % 8)); }

// 設定空間：Linux 先寫 select / subsel 選要問什麼，再讀 size 和 data
static void build_config(vinput_t *in, uint8_t cfg[136]) {
    memset(cfg, 0, 136);
    cfg[0] = in->select;
    cfg[1] = in->subsel;
    uint8_t *data = cfg + 8, size = 0;
    switch (in->select) {
    case CFG_ID_NAME:
        size = (uint8_t)strlen(in->name);
        memcpy(data, in->name, size);
        break;
    case CFG_ID_DEVIDS: {  // 匯流排、廠商、產品、版本
        uint16_t ids[4] = {0x06 /* BUS_VIRTUAL */, 0x4c50, in->pointer ? 2 : 1, 1};
        memcpy(data, ids, 8);
        size = 8;
        break;
    }
    case CFG_EV_BITS:
        if (!in->pointer && in->subsel == EV_KEY) {
            for (unsigned k = 1; k < 256; k++) set_bit(data, k);  // 一般鍵盤上的所有按鍵
            size = 32;
        } else if (!in->pointer && in->subsel == EV_REP) {
            size = 1;  // 回報有這項：Linux 會自己處理「按住不放就連續輸入」
        } else if (in->pointer && in->subsel == EV_KEY) {
            set_bit(data, BTN_LEFT);
            set_bit(data, BTN_RIGHT);
            set_bit(data, BTN_MIDDLE);
            size = BTN_MIDDLE / 8 + 1;
        } else if (in->pointer && in->subsel == EV_ABS) {
            set_bit(data, ABS_X);
            set_bit(data, ABS_Y);
            size = 1;
        } else if (in->pointer && in->subsel == EV_REL) {
            set_bit(data, REL_WHEEL);
            set_bit(data, REL_HWHEEL);
            size = 2;
        }
        break;
    case CFG_ABS_INFO:
        if (in->pointer && (in->subsel == ABS_X || in->subsel == ABS_Y)) {
            uint32_t info[5] = {0, ABS_MAX_VALUE, 0, 0, 0};  // 最小、最大、雜訊、平坦區、解析度
            memcpy(data, info, sizeof info);
            size = sizeof info;
        }
        break;
    }
    cfg[2] = size;
}

static uint64_t input_config_read(virtio_dev_t *d, uint64_t off, unsigned size) {
    vinput_t *in = of(d);
    uint8_t cfg[136];
    pthread_mutex_lock(&in->lock);
    build_config(in, cfg);
    pthread_mutex_unlock(&in->lock);
    uint64_t v = 0;
    if (off + size <= sizeof cfg) memcpy(&v, cfg + off, size);
    return v;
}

static void input_config_write(virtio_dev_t *d, uint64_t off, uint64_t val, unsigned size) {
    (void)size;
    vinput_t *in = of(d);
    pthread_mutex_lock(&in->lock);
    if (off == 0) in->select = (uint8_t)val;
    if (off == 1) in->subsel = (uint8_t)val;
    pthread_mutex_unlock(&in->lock);
}

// 把排隊中的事件放進 Linux 準備好的緩衝區。呼叫前必須持有 in->lock
static void flush(vinput_t *in) {
    virtio_dev_t *d = &in->dev;
    virtq_t *q = &d->vq[EVENTQ];
    static int debug = -1;
    if (debug < 0) debug = getenv("LEAPVM_DEBUG") != NULL;
    if (debug)
        LOG("%s：待送 %d 個事件，佇列 ready=%d num=%u avail=%u 已處理=%u\r\n", d->name, in->npending, q->ready,
            q->num, q->ready ? q->avail->idx : 0, q->last_avail);
    if (!q->ready || !in->npending) return;
    int sent = 0;
    vbuf_t buf[4];
    int n, head;
    while (sent < in->npending && (head = virtq_pop(d, q, buf, 4, &n)) >= 0) {
        vbuf_copy_to(buf, n, 0, &in->pending[sent], sizeof in->pending[0]);
        virtq_push(d, q, head, sizeof in->pending[0]);
        sent++;
    }
    memmove(in->pending, in->pending + sent, (size_t)(in->npending - sent) * sizeof in->pending[0]);
    in->npending -= sent;
    if (sent) virtio_interrupt(d, q);
}

static void push_events(vinput_t *in, const struct input_event *ev, int n) {
    pthread_mutex_lock(&in->lock);
    if (in->npending + n > PENDING_MAX) in->npending = 0;  // Linux 太久沒收：丟掉舊的，免得卡住
    memcpy(in->pending + in->npending, ev, (size_t)n * sizeof *ev);
    in->npending += n;
    flush(in);
    pthread_mutex_unlock(&in->lock);
}

static void input_notify(virtio_dev_t *d, int queue) {
    vinput_t *in = of(d);
    pthread_mutex_lock(&in->lock);
    if (queue == EVENTQ) {
        flush(in);  // Linux 補了新的空緩衝區
    } else {
        // 狀態佇列：Linux 告訴我們鍵盤燈號（大寫鎖定等），我們不需要，收下就好
        virtq_t *q = &d->vq[STATUSQ];
        vbuf_t buf[4];
        int n, head;
        bool any = false;
        while ((head = virtq_pop(d, q, buf, 4, &n)) >= 0) {
            virtq_push(d, q, head, 0);
            any = true;
        }
        if (any) virtio_interrupt(d, q);
    }
    pthread_mutex_unlock(&in->lock);
}

void input_key(int code, bool down) {
    if (code <= 0 || code > 255) return;
    struct input_event ev[2] = {{EV_KEY, (uint16_t)code, down}, {EV_SYN, SYN_REPORT, 0}};
    push_events(&kbd, ev, 2);
}

void input_pointer_move(double x, double y) {
    if (x < 0) x = 0;
    if (x > 1) x = 1;
    if (y < 0) y = 0;
    if (y > 1) y = 1;
    struct input_event ev[3] = {{EV_ABS, ABS_X, (uint32_t)(x * ABS_MAX_VALUE)},
                                {EV_ABS, ABS_Y, (uint32_t)(y * ABS_MAX_VALUE)},
                                {EV_SYN, SYN_REPORT, 0}};
    push_events(&mouse, ev, 3);
}

void input_pointer_button(int button, bool down) {
    static const uint16_t codes[3] = {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE};
    if (button < 0 || button > 2) return;
    struct input_event ev[2] = {{EV_KEY, codes[button], down}, {EV_SYN, SYN_REPORT, 0}};
    push_events(&mouse, ev, 2);
}

void input_pointer_wheel(int dy, int dx) {
    struct input_event ev[3];
    int n = 0;
    if (dy) ev[n++] = (struct input_event){EV_REL, REL_WHEEL, (uint32_t)dy};
    if (dx) ev[n++] = (struct input_event){EV_REL, REL_HWHEEL, (uint32_t)dx};
    if (!n) return;
    ev[n++] = (struct input_event){EV_SYN, SYN_REPORT, 0};
    push_events(&mouse, ev, n);
}

static void init_one(vinput_t *in) {
    in->dev = (virtio_dev_t){
        .name = in->pointer ? "滑鼠" : "鍵盤",
        .device_id = 18,
        .nqueues = 2,
        .queue_max = 64,
        .notify = input_notify,
        .config_read = input_config_read,
        .config_write = input_config_write,
    };
    virtio_register(&in->dev);
}

void virtio_input_init(void) {
    init_one(&kbd);
    init_one(&mouse);
}
