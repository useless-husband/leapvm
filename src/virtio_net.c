// LeapVM：虛擬網路卡（virtio-net）
//
// Linux 送出的網路封包交給 libslirp。libslirp 在 Mac 這邊用一般的網路連線
// 幫 Linux 連到外面（像是家裡的路由器），收到回應再包成封包送回給 Linux。
// 所以不需要管理員權限，Linux 看到的是一個私人網段：
//   Linux 自己   10.0.2.15（透過 DHCP 自動拿到）
//   閘道         10.0.2.2
//   DNS          10.0.2.3
//
// 所有 libslirp 的呼叫都在同一條網路執行緒上，不用擔心同時存取。
// 規格：VIRTIO 1.2 第 5.1 節
#include "virtio.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <poll.h>
#include <resolv.h>
#include <slirp/libslirp.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <string.h>
#include <unistd.h>

#define VIRTIO_NET_F_MTU    (1ULL << 3)
#define VIRTIO_NET_F_MAC    (1ULL << 5)
#define VIRTIO_NET_F_STATUS (1ULL << 16)
#define NET_HDR_LEN 12       // virtio_net_hdr_v1：這個版本不用任何卸載功能，全部填 0
#define RXQ 0
#define TXQ 1
// 一個封包最大多大（MTU）。一般網路是 1500 bytes；虛擬網路沒有實體線路的限制，
// 調到 65520 可以讓同樣的資料少切成 40 多倍的封包，每個封包的固定成本就省下來了
#define MTU 65520
#define MAX_FRAME (MTU + 14)
#define RX_BACKLOG 512       // Linux 還沒準備好接收緩衝區時，最多先暫存這麼多個封包
// 大封包模式下 Linux 每個接收緩衝區要用掉 18 格描述子，佇列要夠大，一次湧進幾百個小封包才接得住
#define NET_QUEUE_SIZE 1024

static const uint8_t mac[6] = {0x52, 0x54, 0x00, 0x4c, 0x45, 0x50};  // 52:54:00 是虛擬機常用的開頭

static struct {
    virtio_dev_t dev;
    Slirp       *slirp;
    int          wake[2];     // 敲門鈴時寫一個 byte 叫醒網路執行緒
    struct pollfd fds[256 + 64];
    int          nfds;
    struct { uint8_t *data; size_t len; } backlog[RX_BACKLOG];
    int          backlog_head, backlog_count;
    bool         rx_irq_due;  // 有新封包放進去了，這一圈結束時要通知 Linux
    struct timer { SlirpTimerId id; void *cb_opaque; int64_t expire_ms; bool armed; } timers[4];
    int          ntimers;
    uint64_t     rx_packets, tx_packets, rx_bytes, tx_bytes, dropped;
} net;

static int64_t now_ns(void) { return (int64_t)host_ticks_to_ns(mach_absolute_time()); }

static bool deliver(const uint8_t *frame, size_t len);
static slirp_ssize_t send_packet(const void *buf, size_t len, void *opaque);

// ── DNS 轉送 ──
// libslirp 內建的 DNS 轉送只看系統設定裡的前三個 DNS 伺服器，而且只用 IPv4；
// 如果那三個剛好都是 IPv6（很多家用網路是這樣），Linux 就查不到任何網址。
// 所以我們自己接手：Linux 問 10.0.2.3 的 DNS 封包，改由 LeapVM 用 Mac 的 DNS 設定
// （IPv4、IPv6 都行）去問，拿到答案再包成「10.0.2.3 回覆的封包」送回給 Linux。
//
// 做法跟真正的 DNS 解析器一樣：只開兩個 socket（IPv4、IPv6 各一），
// 每個查詢換上我們自己的「交易編號」（DNS 封包開頭的 16 位元），
// 回覆回來時靠這個編號找回是誰問的、再把編號換回去。
// 這樣同時可以有上千個查詢在飛，也不會用光檔案描述子。
#define DNS_IP      0x0a000203u  // 10.0.2.3
#define DNS_MAX     1024
#define DNS_TIMEOUT_MS 2000
#define DNS_REPLY_MAX 4096   // EDNS 的回覆可以到 4KB；虛擬網路的 MTU 夠大，一個封包裝得下

static const uint8_t dns_mac[6] = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x03};  // libslirp 給 10.0.2.3 用的 MAC

static struct {
    struct sockaddr_storage addr[6];
    socklen_t len[6];
    int       n;
    int64_t   loaded_ms;
    int       sock4, sock6;    // 0 = 還沒開
    uint16_t  salt;            // 交易編號 = 格子編號 XOR salt，每次開機不同
    int       pending;
    int       preferred;       // 上次有回應的伺服器：新的查詢先問它（家用路由器的 DNS 常常慢）
    uint64_t  timeouts, stale; // 統計：伺服器沒回應的次數、遲到／對不上的回覆
} dns_servers;

static struct dns_query {
    bool     in_use;
    uint16_t orig_id;          // Linux 原本的交易編號
    int      server;           // 目前問的伺服器
    int      tries;            // 已經試過幾個伺服器
    int64_t  deadline_ms;
    uint8_t  guest_mac[6];
    uint32_t guest_ip;
    uint16_t guest_port;
    uint16_t len;
    uint8_t  data[512];
} dns_q[DNS_MAX];

static void load_dns_servers(int64_t now_ms) {
    if (dns_servers.n && now_ms - dns_servers.loaded_ms < 30000) return;  // 30 秒內用快取
    dns_servers.n = 0;
    dns_servers.preferred = 0;
    dns_servers.loaded_ms = now_ms;
    struct __res_state st;
    memset(&st, 0, sizeof st);
    if (res_ninit(&st) == 0) {
        union res_sockaddr_union u[8];
        int n = res_getservers(&st, u, 8);
        for (int i = 0; i < n && dns_servers.n < 5; i++) {
            if (u[i].sin.sin_family == AF_INET) {
                memcpy(&dns_servers.addr[dns_servers.n], &u[i].sin, sizeof u[i].sin);
                dns_servers.len[dns_servers.n++] = sizeof u[i].sin;
            } else if (u[i].sin6.sin6_family == AF_INET6) {
                memcpy(&dns_servers.addr[dns_servers.n], &u[i].sin6, sizeof u[i].sin6);
                dns_servers.len[dns_servers.n++] = sizeof u[i].sin6;
            }
        }
        res_ndestroy(&st);
    }
    // 萬一系統沒有設定任何 DNS，最後再試公共 DNS
    struct sockaddr_in fallback = {.sin_family = AF_INET, .sin_port = htons(53)};
    inet_pton(AF_INET, "1.1.1.1", &fallback.sin_addr);
    memcpy(&dns_servers.addr[dns_servers.n], &fallback, sizeof fallback);
    dns_servers.len[dns_servers.n++] = sizeof fallback;
}

static int dns_socket(int family) {
    int *slot = family == AF_INET6 ? &dns_servers.sock6 : &dns_servers.sock4;
    if (!*slot) {
        *slot = socket(family, SOCK_DGRAM, 0);
        if (*slot > 0) fcntl(*slot, F_SETFL, O_NONBLOCK);
    }
    return *slot;
}

// 把第 i 格的查詢送給它目前排到的伺服器；伺服器都試完了就放棄，回傳 false
static bool dns_send(int i, int64_t now_ms) {
    struct dns_query *q = &dns_q[i];
    while (q->tries < dns_servers.n) {
        q->server %= dns_servers.n;  // 從偏好的伺服器開始，繞一圈
        const struct sockaddr *sa = (const struct sockaddr *)&dns_servers.addr[q->server];
        int fd = dns_socket(sa->sa_family);
        uint16_t id = (uint16_t)i ^ dns_servers.salt;
        q->data[0] = (uint8_t)(id >> 8);
        q->data[1] = (uint8_t)id;
        if (fd > 0 && sendto(fd, q->data, q->len, 0, sa, dns_servers.len[q->server]) == q->len) {
            q->deadline_ms = now_ms + DNS_TIMEOUT_MS;
            return true;
        }
        q->server++;
        q->tries++;
    }
    q->in_use = false;
    dns_servers.pending--;
    return false;
}

static uint16_t ip_checksum(const uint8_t *p, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < len; i += 2) sum += (uint32_t)(p[i] << 8 | p[i + 1]);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

// 把 DNS 答案包成「10.0.2.3:53 → Linux」的乙太網路封包送回去
static void dns_reply(const struct dns_query *q, uint8_t *payload, size_t len) {
    uint8_t f[14 + 20 + 8 + DNS_REPLY_MAX];
    if (len < 2 || len > DNS_REPLY_MAX) return;
    payload[0] = (uint8_t)(q->orig_id >> 8);  // 交易編號換回 Linux 原本的
    payload[1] = (uint8_t)q->orig_id;
    memcpy(f, q->guest_mac, 6);
    memcpy(f + 6, dns_mac, 6);
    f[12] = 0x08; f[13] = 0x00;                         // IPv4
    uint8_t *ip = f + 14, *udp = ip + 20;
    size_t ip_len = 20 + 8 + len;
    memset(ip, 0, 20);
    ip[0] = 0x45;
    ip[2] = (uint8_t)(ip_len >> 8); ip[3] = (uint8_t)ip_len;
    ip[8] = 64;                                         // TTL
    ip[9] = 17;                                         // UDP
    uint32_t src = DNS_IP, dst = q->guest_ip;
    for (int i = 0; i < 4; i++) { ip[12 + i] = (uint8_t)(src >> (24 - 8 * i)); ip[16 + i] = (uint8_t)(dst >> (24 - 8 * i)); }
    uint16_t c = ip_checksum(ip, 20);
    ip[10] = (uint8_t)(c >> 8); ip[11] = (uint8_t)c;
    udp[0] = 0; udp[1] = 53;
    udp[2] = (uint8_t)(q->guest_port >> 8); udp[3] = (uint8_t)q->guest_port;
    udp[4] = (uint8_t)((8 + len) >> 8); udp[5] = (uint8_t)(8 + len);
    udp[6] = udp[7] = 0;                                // IPv4 的 UDP 可以不填檢查碼
    memcpy(udp + 8, payload, len);
    send_packet(f, 14 + ip_len, NULL);
}

// Linux 送出的封包如果是「問 10.0.2.3 的 DNS」，就由我們處理，回傳 true
static bool dns_intercept(const uint8_t *f, size_t len) {
    if (len < 14 + 20 + 8 || f[12] != 0x08 || f[13] != 0x00) return false;
    const uint8_t *ip = f + 14;
    size_t ihl = (size_t)(ip[0] & 0x0f) * 4;
    if ((ip[0] >> 4) != 4 || ip[9] != 17 || len < 14 + ihl + 8) return false;
    uint32_t dst = (uint32_t)ip[16] << 24 | (uint32_t)ip[17] << 16 | (uint32_t)ip[18] << 8 | ip[19];
    const uint8_t *udp = ip + ihl;
    if (dst != DNS_IP || (udp[2] << 8 | udp[3]) != 53) return false;
    size_t plen = (size_t)(udp[4] << 8 | udp[5]);
    if (plen < 8 + 2 || 14 + ihl + plen > len || plen - 8 > sizeof dns_q[0].data) return true;  // 格式不對：丟掉

    int64_t now_ms = now_ns() / 1000000;
    load_dns_servers(now_ms);
    if (!dns_servers.salt) dns_servers.salt = (uint16_t)(mach_absolute_time() | 1);
    static int next_slot;  // 從上次之後開始找，格子平均使用
    for (int k = 0; k < DNS_MAX; k++) {
        int i = (next_slot + k) % DNS_MAX;
        struct dns_query *q = &dns_q[i];
        if (q->in_use) continue;
        next_slot = i + 1;
        memcpy(q->guest_mac, f + 6, 6);
        q->guest_ip = (uint32_t)ip[12] << 24 | (uint32_t)ip[13] << 16 | (uint32_t)ip[14] << 8 | ip[15];
        q->guest_port = (uint16_t)(udp[0] << 8 | udp[1]);
        q->len = (uint16_t)(plen - 8);
        memcpy(q->data, udp + 8, q->len);
        q->orig_id = (uint16_t)(q->data[0] << 8 | q->data[1]);
        q->server = dns_servers.preferred;
        q->tries = 0;
        q->in_use = true;
        dns_servers.pending++;
        dns_send(i, now_ms);
        return true;
    }
    net.dropped++;
    return true;  // 同時超過 1024 個查詢：丟掉，Linux 會重試
}

// 網路執行緒每一圈都呼叫：處理逾時、把 DNS 的 socket 加進 poll 清單
static void dns_poll_add(struct pollfd *fds, int *nfds, int *map, uint32_t *timeout) {
    map[0] = map[1] = -1;
    if (!dns_servers.pending) return;
    int64_t now_ms = now_ns() / 1000000;
    for (int i = 0; i < DNS_MAX; i++) {
        struct dns_query *q = &dns_q[i];
        if (!q->in_use) continue;
        if (now_ms >= q->deadline_ms) {  // 這個伺服器沒回應：換下一個
            dns_servers.timeouts++;
            q->server++;
            q->tries++;
            if (!dns_send(i, now_ms)) continue;
        }
        int64_t left = q->deadline_ms - now_ms;
        if ((uint64_t)left < *timeout) *timeout = (uint32_t)left;
    }
    if (dns_servers.sock4 > 0) { map[0] = *nfds; fds[(*nfds)++] = (struct pollfd){.fd = dns_servers.sock4, .events = POLLIN}; }
    if (dns_servers.sock6 > 0) { map[1] = *nfds; fds[(*nfds)++] = (struct pollfd){.fd = dns_servers.sock6, .events = POLLIN}; }
}

static void dns_poll_done(const struct pollfd *fds, const int *map) {
    uint8_t buf[DNS_REPLY_MAX];
    for (int s = 0; s < 2; s++) {
        if (map[s] < 0 || !(fds[map[s]].revents & POLLIN)) continue;
        for (;;) {  // 一次把收到的回覆全部拿完
            ssize_t n = recv(fds[map[s]].fd, buf, sizeof buf, 0);
            if (n < 2) break;
            int i = ((buf[0] << 8 | buf[1]) ^ dns_servers.salt) & (DNS_MAX - 1);
            struct dns_query *q = &dns_q[i];
            // 過期或亂送的回覆：格子已經空了、編號對不上、或問的問題不一樣（格子被新的查詢重複使用了）
            size_t qlen = q->len > 12 ? q->len - 12 : 0;
            if (!q->in_use || (uint16_t)(i ^ dns_servers.salt) != (uint16_t)(buf[0] << 8 | buf[1]) ||
                (size_t)n < 12 + qlen || memcmp(buf + 12, q->data + 12, qlen) != 0) {
                dns_servers.stale++;
                continue;
            }
            dns_servers.preferred = q->server;  // 這台有回應，之後先問它
            dns_reply(q, buf, (size_t)n);
            q->in_use = false;
            dns_servers.pending--;
        }
    }
}

// ── 收：libslirp 要送一個封包給 Linux ──
static bool deliver(const uint8_t *frame, size_t len) {
    virtio_dev_t *d = &net.dev;
    virtq_t *q = &d->vq[RXQ];
    vbuf_t bufs[32];  // 大封包模式下，Linux 一個接收緩衝區由最多 19 段記憶體串成
    int n;
    if (!q->ready) return false;
    int head = virtq_pop(d, q, bufs, 32, &n);
    if (head < 0) return false;  // Linux 還沒放空的接收緩衝區進來
    uint8_t hdr[NET_HDR_LEN] = {0};
    hdr[10] = 1;  // num_buffers = 1
    size_t w = vbuf_copy_to(bufs, n, 0, hdr, NET_HDR_LEN);
    w += vbuf_copy_to(bufs, n, NET_HDR_LEN, frame, len);
    if (w < NET_HDR_LEN + len) {  // 緩衝區太小：把緩衝區還給 Linux 但標成 0 bytes，不要送半個封包
        net.dropped++;
        w = 0;
    }
    virtq_push(d, q, head, (uint32_t)w);
    net.rx_packets++;
    net.rx_bytes += len;
    return true;
}

static void flush_backlog(void) {
    bool any = false;
    while (net.backlog_count) {
        typeof(net.backlog[0]) *b = &net.backlog[net.backlog_head];
        if (!deliver(b->data, b->len)) break;
        free(b->data);
        net.backlog_head = (net.backlog_head + 1) % RX_BACKLOG;
        net.backlog_count--;
        any = true;
    }
    if (any) net.rx_irq_due = true;
}

// 收到的封包先放進 Linux 的接收佇列，但先不通知；網路執行緒每一圈最後才通知一次，
// 這樣一大批封包只需要一次中斷
static slirp_ssize_t send_packet(const void *buf, size_t len, void *opaque) {
    (void)opaque;
    if (!net.backlog_count && deliver(buf, len)) {
        net.rx_irq_due = true;
        return (slirp_ssize_t)len;
    }
    // Linux 暫時收不下：先存起來，等它補充接收緩衝區（會敲 RX 的門鈴）再送
    if (net.backlog_count == RX_BACKLOG) {
        net.dropped++;
        return (slirp_ssize_t)len;  // 丟掉，TCP 會自己重傳
    }
    int i = (net.backlog_head + net.backlog_count) % RX_BACKLOG;
    net.backlog[i].data = malloc(len);
    memcpy(net.backlog[i].data, buf, len);
    net.backlog[i].len = len;
    net.backlog_count++;
    return (slirp_ssize_t)len;
}

// ── 送：Linux 放了要送出去的封包 ──
static void process_tx(void) {
    virtio_dev_t *d = &net.dev;
    virtq_t *q = &d->vq[TXQ];
    static uint8_t frame[MAX_FRAME];
    vbuf_t bufs[32];
    bool did = false;
    if (!q->ready) return;
    do {
        virtq_disable_notify(q);
        int n, head;
        while ((head = virtq_pop(d, q, bufs, 32, &n)) >= 0) {
            size_t len = vbuf_copy_from(bufs, n, NET_HDR_LEN, frame, sizeof frame);
            if (!dns_intercept(frame, len)) slirp_input(net.slirp, frame, (int)len);
            virtq_push(d, q, head, 0);
            net.tx_packets++;
            net.tx_bytes += len;
            did = true;
        }
    } while (virtq_enable_notify(q));
    if (did) virtio_interrupt(d, q);
}

// ── libslirp 需要的其他回呼 ──
static void guest_error(const char *msg, void *opaque) {
    (void)opaque;
    LOG("網路：%s\r\n", msg);
}

static int64_t clock_get_ns(void *opaque) {
    (void)opaque;
    return now_ns();
}

static void *timer_new_opaque(SlirpTimerId id, void *cb_opaque, void *opaque) {
    (void)opaque;
    if (net.ntimers == 4) die("網路計時器太多\n");
    struct timer *t = &net.timers[net.ntimers++];
    *t = (struct timer){.id = id, .cb_opaque = cb_opaque};
    return t;
}

static void timer_free(void *timer, void *opaque) {
    (void)opaque;
    ((struct timer *)timer)->armed = false;
}

static void timer_mod(void *timer, int64_t expire_ms, void *opaque) {
    (void)opaque;
    struct timer *t = timer;
    t->expire_ms = expire_ms;
    t->armed = true;
}

static void register_poll_socket(slirp_os_socket fd, void *opaque) { (void)fd; (void)opaque; }
static void unregister_poll_socket(slirp_os_socket fd, void *opaque) { (void)fd; (void)opaque; }
static void notify(void *opaque) {
    (void)opaque;
    (void)!write(net.wake[1], "n", 1);
}

static int add_poll(slirp_os_socket fd, int events, void *opaque) {
    (void)opaque;
    if (net.nfds == 256 + 64) return -1;
    short ev = 0;
    if (events & SLIRP_POLL_IN) ev |= POLLIN;
    if (events & SLIRP_POLL_OUT) ev |= POLLOUT;
    if (events & SLIRP_POLL_PRI) ev |= POLLPRI;
    net.fds[net.nfds] = (struct pollfd){.fd = fd, .events = ev};
    return net.nfds++;
}

static int get_revents(int idx, void *opaque) {
    (void)opaque;
    if (idx < 0 || idx >= net.nfds) return 0;
    short r = net.fds[idx].revents;
    int ev = 0;
    if (r & POLLIN) ev |= SLIRP_POLL_IN;
    if (r & POLLOUT) ev |= SLIRP_POLL_OUT;
    if (r & POLLPRI) ev |= SLIRP_POLL_PRI;
    if (r & POLLERR) ev |= SLIRP_POLL_ERR;
    if (r & POLLHUP) ev |= SLIRP_POLL_HUP;
    return ev;
}

static const SlirpCb callbacks = {
    .send_packet = send_packet,
    .guest_error = guest_error,
    .clock_get_ns = clock_get_ns,
    .timer_free = timer_free,
    .timer_mod = timer_mod,
    .notify = notify,
    .timer_new_opaque = timer_new_opaque,
    .register_poll_socket = register_poll_socket,
    .unregister_poll_socket = unregister_poll_socket,
};

static void *net_thread(void *arg) {
    (void)arg;
    for (;;) {
        uint32_t timeout = 1000;
        int64_t now_ms = now_ns() / 1000000;
        for (int i = 0; i < net.ntimers; i++) {
            struct timer *t = &net.timers[i];
            if (!t->armed) continue;
            int64_t left = t->expire_ms - now_ms;
            if (left < 0) left = 0;
            if ((uint64_t)left < timeout) timeout = (uint32_t)left;
        }
        int dns_map[2];
        net.nfds = 0;
        net.fds[net.nfds++] = (struct pollfd){.fd = net.wake[0], .events = POLLIN};
        dns_poll_add(net.fds, &net.nfds, dns_map, &timeout);
        slirp_pollfds_fill_socket(net.slirp, &timeout, add_poll, NULL);
        int r = poll(net.fds, (nfds_t)net.nfds, (int)timeout);
        if (r > 0 && (net.fds[0].revents & POLLIN)) {
            char drain[64];
            while (read(net.wake[0], drain, sizeof drain) > 0) {}
        }
        if (r > 0) dns_poll_done(net.fds, dns_map);
        slirp_pollfds_poll(net.slirp, r < 0, get_revents, NULL);

        now_ms = now_ns() / 1000000;
        for (int i = 0; i < net.ntimers; i++) {
            struct timer *t = &net.timers[i];
            if (t->armed && t->expire_ms <= now_ms) {
                t->armed = false;
                slirp_handle_timer(net.slirp, t->id, t->cb_opaque);
            }
        }
        process_tx();
        flush_backlog();
        if (net.rx_irq_due) {
            net.rx_irq_due = false;
            virtio_interrupt(&net.dev, &net.dev.vq[RXQ]);
        }
    }
    return NULL;
}

static void net_notify(virtio_dev_t *d, int queue) {
    (void)d;
    (void)queue;
    (void)!write(net.wake[1], "q", 1);  // TX 有新封包，或 RX 補了緩衝區：叫醒網路執行緒
}

static uint64_t net_config_read(virtio_dev_t *d, uint64_t off, unsigned size) {
    (void)d;
    uint8_t cfg[12] = {0};
    memcpy(cfg, mac, 6);
    cfg[6] = 1;  // status：連線中
    uint16_t mtu = MTU;
    memcpy(cfg + 10, &mtu, 2);
    uint64_t v = 0;
    if (off + size <= sizeof cfg) memcpy(&v, cfg + off, size);
    return v;
}

static void net_print_stats(virtio_dev_t *d) {
    (void)d;
    fprintf(stderr, "｜收 %llu 包 %.1f MB｜送 %llu 包 %.1f MB｜丟棄 %llu｜暫存 %d｜DNS 等待中 %d 逾時 %llu 對不上 %llu",
            net.rx_packets, net.rx_bytes / 1048576.0, net.tx_packets, net.tx_bytes / 1048576.0, net.dropped,
            net.backlog_count, dns_servers.pending, dns_servers.timeouts, dns_servers.stale);
}

static void net_reset(virtio_dev_t *d) {
    (void)d;
    while (net.backlog_count) {
        free(net.backlog[net.backlog_head].data);
        net.backlog_head = (net.backlog_head + 1) % RX_BACKLOG;
        net.backlog_count--;
    }
}

void virtio_net_init(void) {
    if (pipe(net.wake)) die("建立網路執行緒的通道失敗\n");
    fcntl(net.wake[0], F_SETFL, O_NONBLOCK);
    fcntl(net.wake[1], F_SETFL, O_NONBLOCK);

    SlirpConfig cfg = {.version = 6, .in_enabled = true};  // 版本 6 才會用 register_poll_socket（舊版回呼我們沒提供）
    inet_pton(AF_INET, "10.0.2.0", &cfg.vnetwork);
    inet_pton(AF_INET, "255.255.255.0", &cfg.vnetmask);
    inet_pton(AF_INET, "10.0.2.2", &cfg.vhost);
    inet_pton(AF_INET, "10.0.2.15", &cfg.vdhcp_start);
    inet_pton(AF_INET, "10.0.2.3", &cfg.vnameserver);
    cfg.vhostname = "leapvm";
    cfg.if_mtu = cfg.if_mru = MTU;
    net.slirp = slirp_new(&cfg, &callbacks, NULL);
    if (!net.slirp) die("網路初始化失敗\n");

    net.dev = (virtio_dev_t){
        .name = "網路卡",
        .device_id = 1,
        .features = VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS | VIRTIO_NET_F_MTU,
        .nqueues = 2,
        .queue_max = NET_QUEUE_SIZE,
        .notify = net_notify,
        .config_read = net_config_read,
        .reset = net_reset,
        .print_stats = net_print_stats,
    };
    virtio_register(&net.dev);
    pthread_t t;
    pthread_create(&t, NULL, net_thread, NULL);
    pthread_detach(t);
    LOG("網路卡：%02x:%02x:%02x:%02x:%02x:%02x，經由 libslirp 連到 Mac 的網路\n",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
