// LeapVM：虛擬 CPU
//
// 每顆 vCPU 是一條主機執行緒，不斷重複「進入虛擬機執行 → 因為某件事離開 → 處理 → 再進入」。
// Linux 的程式在虛擬機裡是直接用真的 CPU 在跑，只有碰到下面這幾種事情才會離開：
//   - 讀寫裝置（序列埠、硬碟、網路卡……）  → 交給裝置登記表裡對應的裝置
//   - 用 HVC 指令呼叫 PSCI（開關 CPU、關機）→ handle_psci
//   - 沒事做，執行 WFI 等中斷              → 讓主機執行緒睡一下，不要空轉
// 離開的次數越少，效能損失就越小。
#include "vm.h"
#include <mach/mach_time.h>
#include <string.h>
#include <unistd.h>

#define PSTATE_EL1H_MASKED 0x3c5  // EL1、使用 SP_EL1、所有中斷遮蔽：CPU 剛開機的狀態

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;  // 管 vCPU 的開關機與閒置喚醒
static pthread_cond_t  cond = PTHREAD_COND_INITIALIZER;
static bool started;                 // vm_start() 之前所有 vCPU 停在起跑線
static uint64_t wake_seq;            // 每次有新中斷就 +1，閒置中的 vCPU 靠它判斷要不要醒
static uint64_t vtimer_offset;       // 虛擬機時鐘 = 主機時鐘 - offset
static mach_timebase_info_data_t timebase;

uint64_t host_ticks_to_ns(uint64_t ticks) {
    if (!timebase.denom) mach_timebase_info(&timebase);
    return ticks * timebase.numer / timebase.denom;
}

static uint64_t ns_to_host_ticks(uint64_t ns) {
    if (!timebase.denom) mach_timebase_info(&timebase);
    return ns * timebase.denom / timebase.numer;
}

// ── Mac 睡眠 ──
// 虛擬機的時鐘是「主機的 mach_absolute_time 減一個偏移」，而 mach_absolute_time 在 Mac 睡眠時不會走。
// 所以蓋上蓋子一小時再打開，Linux 的時間會慢一小時。mach_continuous_time 會把睡眠時間算進去，
// 每秒比對兩者的差距，差距突然變大就代表剛睡醒，把偏移往回撥、讓虛擬機的時鐘一次跳到正確的時間。
static uint64_t sleep_seen;  // 上次看到的 continuous − absolute

static void clock_adjust_locked(uint64_t forward_ticks) {
    vtimer_offset -= forward_ticks;
    for (int i = 0; i < vm.ncpus; i++) {
        vm.cpus[i].offset_dirty = true;
        hv_vcpus_exit(&vm.cpus[i].hv, 1);  // 叫出來套用新的偏移
    }
}

void vm_clock_forward(uint64_t seconds) {
    pthread_mutex_lock(&lock);
    clock_adjust_locked(ns_to_host_ticks(seconds * 1000000000ULL));
    pthread_mutex_unlock(&lock);
}

static void *sleep_watch_thread(void *arg) {
    (void)arg;
    for (;;) {
        sleep(1);
        uint64_t drift = mach_continuous_time() - mach_absolute_time();
        pthread_mutex_lock(&lock);
        if (started && drift > sleep_seen + ns_to_host_ticks(500 * 1000000ULL)) {
            uint64_t slept = drift - sleep_seen;
            LOG("Mac 睡了 %.0f 秒，把虛擬機的時鐘往前撥\r\n", host_ticks_to_ns(slept) / 1e9);
            clock_adjust_locked(slept);
        }
        sleep_seen = drift;
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

// ── 裝置登記表 ──
#define MAX_MMIO 32
static mmio_device_t mmio[MAX_MMIO];
static int nmmio;

void mmio_register(const mmio_device_t *dev) {
    if (nmmio == MAX_MMIO) die("裝置太多\n");
    mmio[nmmio++] = *dev;
}

const mmio_device_t *mmio_find(uint64_t addr) {
    for (int i = 0; i < nmmio; i++)
        if (addr >= mmio[i].base && addr - mmio[i].base < mmio[i].size) return &mmio[i];
    return NULL;
}

// ── 暫存器存取（Rt = 31 代表零暫存器 XZR）──
static uint64_t get_x(vcpu_t *c, unsigned r) {
    uint64_t v = 0;
    if (r < 31) HV(hv_vcpu_get_reg(c->hv, HV_REG_X0 + r, &v));
    return v;
}

static void set_x(vcpu_t *c, unsigned r, uint64_t v) {
    if (r < 31) HV(hv_vcpu_set_reg(c->hv, HV_REG_X0 + r, v));
}

static void advance_pc(vcpu_t *c) {
    uint64_t pc;
    HV(hv_vcpu_get_reg(c->hv, HV_REG_PC, &pc));
    HV(hv_vcpu_set_reg(c->hv, HV_REG_PC, pc + 4));
}

// ── 閒置等待（WFI）──
// Linux 沒事做時會執行 WFI「等中斷」。我們讓主機執行緒睡到計時器到期、
// 或有裝置送中斷進來為止，避免空轉吃滿主機 CPU。
static void wait_for_interrupt(vcpu_t *c) {
    uint64_t ctl, cval;
    HV(hv_vcpu_get_sys_reg(c->hv, HV_SYS_REG_CNTV_CTL_EL0, &ctl));
    HV(hv_vcpu_get_sys_reg(c->hv, HV_SYS_REG_CNTV_CVAL_EL0, &cval));

    // 其他 vCPU 送來的中斷（IPI）由系統內的中斷控制器直接處理，我們收不到通知，
    // 所以多核心時最多睡 1ms 就回去看一次
    uint64_t wait_ns = vm.ncpus > 1 ? 1000 * 1000 : 10 * 1000 * 1000;

    pthread_mutex_lock(&lock);
    if ((ctl & 1) && !(ctl & 2)) {  // 計時器啟用且沒被遮蔽
        uint64_t now = mach_absolute_time() - vtimer_offset;
        if (cval <= now) { pthread_mutex_unlock(&lock); return; }
        uint64_t ns = host_ticks_to_ns(cval - now);
        if (ns < wait_ns) wait_ns = ns;
    }
    // 上次醒來之後如果又有人送過中斷，就不睡了，直接回虛擬機處理
    if (c->wake_seen == wake_seq) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = (long)wait_ns};
        pthread_cond_timedwait_relative_np(&cond, &lock, &ts);
    }
    c->wake_seen = wake_seq;
    pthread_mutex_unlock(&lock);
}

void vm_kick_all(void) {
    pthread_mutex_lock(&lock);
    wake_seq++;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock);
}

static _Noreturn void dump_and_die(vcpu_t *c, const char *why, uint64_t syndrome) {
    uint64_t pc = 0, cpsr = 0, far = 0;
    hv_vcpu_get_reg(c->hv, HV_REG_PC, &pc);
    hv_vcpu_get_reg(c->hv, HV_REG_CPSR, &cpsr);
    hv_vcpu_get_sys_reg(c->hv, HV_SYS_REG_FAR_EL1, &far);
    die("CPU %d %s：ESR=%#llx（EC=%#llx）PC=%#llx CPSR=%#llx FAR=%#llx IPA=%#llx\n", c->index, why,
        syndrome, (syndrome >> 26) & 0x3f, pc, cpsr, far, c->exit->exception.physical_address);
}

// Data Abort：Linux 讀寫了一個沒有對應記憶體的位址，也就是某個裝置的暫存器
static void handle_data_abort(vcpu_t *c, uint64_t syndrome) {
    uint64_t ipa = c->exit->exception.physical_address;
    uint32_t iss = syndrome & 0x1ffffff;
    if (!(iss & (1u << 24))) dump_and_die(c, "無法解碼的裝置存取（ISV=0）", syndrome);
    unsigned size = 1u << ((iss >> 22) & 3);  // SAS：存取大小
    bool sign_ext = iss & (1u << 21);         // SSE：是否要符號延伸
    unsigned rt = (iss >> 16) & 31;           // SRT：用到哪個暫存器
    bool is64 = iss & (1u << 15);             // SF：目標暫存器是 64 位元
    bool write = iss & (1u << 6);             // WnR：寫入還是讀取

    const mmio_device_t *dev = mmio_find(ipa);
    if (write) {
        c->stats.mmio_write++;
        uint64_t v = get_x(c, rt);
        if (size < 8) v &= (1ULL << (size * 8)) - 1;
        if (dev) dev->write(dev->opaque, ipa - dev->base, v, size);
    } else {
        c->stats.mmio_read++;
        uint64_t v = dev ? dev->read(dev->opaque, ipa - dev->base, size) : 0;
        if (sign_ext && size < 8) {
            unsigned shift = 64 - size * 8;
            v = (uint64_t)((int64_t)(v << shift) >> shift);
        }
        if (!is64) v &= 0xffffffffULL;
        set_x(c, rt, v);
    }
    if (!dev) {
        static atomic_int warned;
        if (atomic_fetch_add(&warned, 1) < 8)
            LOG("%s不存在的裝置位址 %#llx（%u bytes）\r\n", write ? "寫入" : "讀取", ipa, size);
    }
    advance_pc(c);
}

// ── PSCI：Linux 透過 HVC 指令請我們開關 CPU、關機 ──
enum {
    PSCI_VERSION = 0x84000000, PSCI_CPU_OFF = 0x84000002, PSCI_CPU_ON = 0xc4000003,
    PSCI_AFFINITY_INFO = 0xc4000004, PSCI_MIGRATE_INFO_TYPE = 0x84000006,
    PSCI_SYSTEM_OFF = 0x84000008, PSCI_SYSTEM_RESET = 0x84000009, PSCI_FEATURES = 0x8400000a,
};
#define PSCI_SUCCESS        0ULL
#define PSCI_NOT_SUPPORTED  ((uint64_t)-1)
#define PSCI_INVALID_PARAMS ((uint64_t)-2)
#define PSCI_ALREADY_ON     ((uint64_t)-4)

static uint64_t psci_cpu_on(uint64_t mpidr, uint64_t entry, uint64_t context) {
    uint64_t target = mpidr & 0xff00ffffffULL;
    if (target >= (uint64_t)vm.ncpus) return PSCI_INVALID_PARAMS;
    vcpu_t *t = &vm.cpus[target];

    pthread_mutex_lock(&lock);
    if (t->powered_on) {
        pthread_mutex_unlock(&lock);
        return PSCI_ALREADY_ON;
    }
    t->start_pc = entry;
    t->start_x0 = context;
    t->start_pending = true;
    t->powered_on = true;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock);
    return PSCI_SUCCESS;
}

static void handle_psci(vcpu_t *c) {
    uint32_t fn = (uint32_t)get_x(c, 0);
    uint64_t ret = PSCI_NOT_SUPPORTED;
    switch (fn) {
    case PSCI_VERSION: ret = 0x00010001; break;  // 1.1
    case PSCI_MIGRATE_INFO_TYPE: ret = 2; break;  // 沒有 Trusted OS
    case PSCI_FEATURES:
        switch ((uint32_t)get_x(c, 1)) {
        case PSCI_VERSION: case PSCI_CPU_OFF: case PSCI_CPU_ON: case PSCI_AFFINITY_INFO:
        case PSCI_MIGRATE_INFO_TYPE: case PSCI_SYSTEM_OFF: case PSCI_SYSTEM_RESET: case PSCI_FEATURES:
            ret = PSCI_SUCCESS;
        }
        break;
    case PSCI_CPU_ON:
        ret = psci_cpu_on(get_x(c, 1), get_x(c, 2), get_x(c, 3));
        break;
    case PSCI_CPU_OFF:
        pthread_mutex_lock(&lock);
        c->powered_on = false;
        pthread_mutex_unlock(&lock);
        return;  // 主迴圈看到 powered_on = false 就會停下來等
    case PSCI_AFFINITY_INFO: {
        uint64_t target = get_x(c, 1) & 0xff00ffffffULL;
        if (target >= (uint64_t)vm.ncpus) { ret = PSCI_INVALID_PARAMS; break; }
        pthread_mutex_lock(&lock);
        ret = vm.cpus[target].powered_on ? 0 : 1;  // 0 = ON，1 = OFF
        pthread_mutex_unlock(&lock);
        break;
    }
    case PSCI_SYSTEM_OFF:
    case PSCI_SYSTEM_RESET:
        restore_terminal();
        LOG("Linux 要求%s，虛擬機結束\n", fn == PSCI_SYSTEM_OFF ? "關機" : "重新開機");
        print_stats();
        vm_exit(0);
    }
    set_x(c, 0, ret);
}

// 系統暫存器陷阱：Linux 碰了虛擬機不開放的系統暫存器（例如除錯鎖、效能計數器），
// 讀一律回 0、寫一律忽略
static void handle_sysreg(vcpu_t *c, uint32_t iss) {
    bool read = iss & 1;
    unsigned rt = (iss >> 5) & 31;
    if (read) set_x(c, rt, 0);
    advance_pc(c);
}

// 虛擬計時器到期後會被框架自動遮蔽；等 Linux 處理完（不再是觸發狀態）才解除
static void sync_vtimer(vcpu_t *c) {
    if (!c->vtimer_masked) return;
    uint64_t ctl;
    HV(hv_vcpu_get_sys_reg(c->hv, HV_SYS_REG_CNTV_CTL_EL0, &ctl));
    bool firing = (ctl & 1) && !(ctl & 2) && (ctl & 4);  // 啟用、未遮蔽、已觸發
    if (!firing) {
        HV(hv_vcpu_set_vtimer_mask(c->hv, false));
        c->vtimer_masked = false;
    }
}

static void handle_exit(vcpu_t *c) {
    switch (c->exit->reason) {
    case HV_EXIT_REASON_EXCEPTION: {
        uint64_t syndrome = c->exit->exception.syndrome;
        switch ((syndrome >> 26) & 0x3f) {
        case 0x24:  // Data Abort
            handle_data_abort(c, syndrome);
            break;
        case 0x16:  // HVC：PC 已經指向下一行
            c->stats.hvc++;
            handle_psci(c);
            break;
        case 0x17:  // SMC：要自己跳過這行
            c->stats.hvc++;
            handle_psci(c);
            advance_pc(c);
            break;
        case 0x01:  // WFI / WFE
            c->stats.wfi++;
            advance_pc(c);
            if (!(syndrome & 1)) wait_for_interrupt(c);
            break;
        case 0x18:  // 系統暫存器
            c->stats.sysreg++;
            handle_sysreg(c, syndrome & 0x1ffffff);
            break;
        default:
            dump_and_die(c, "遇到無法處理的例外", syndrome);
        }
        break;
    }
    case HV_EXIT_REASON_VTIMER_ACTIVATED:
        c->stats.vtimer++;
        c->vtimer_masked = true;
        HV(hv_gic_set_redistributor_reg(c->hv, HV_GIC_REDISTRIBUTOR_REG_GICR_ISPENDR0, 1u << VTIMER_PPI));
        break;
    case HV_EXIT_REASON_CANCELED:
        break;
    default:
        die("CPU %d：未知的 VM exit 原因 %d\n", c->index, c->exit->reason);
    }
}

static void *vcpu_thread(void *arg) {
    vcpu_t *c = arg;
    HV(hv_vcpu_create(&c->hv, &c->exit, NULL));
    // MPIDR 的低位是 CPU 編號，必須跟硬體清單裡的 cpu@N 一致；bit 31 依規格固定為 1
    HV(hv_vcpu_set_sys_reg(c->hv, HV_SYS_REG_MPIDR_EL1, (1ULL << 31) | (uint64_t)c->index));
    cpu_features_apply(c->hv);

    pthread_mutex_lock(&lock);
    c->created = true;
    pthread_cond_broadcast(&cond);

    for (;;) {
        // 還沒開始、或這顆 CPU 被關掉了：等著
        while (!started || !c->powered_on) pthread_cond_wait(&cond, &lock);
        if (c->start_pending) {  // 開機或 CPU_ON：從指定的位址開始跑
            c->start_pending = false;
            HV(hv_vcpu_set_reg(c->hv, HV_REG_PC, c->start_pc));
            HV(hv_vcpu_set_reg(c->hv, HV_REG_X0, c->start_x0));
            HV(hv_vcpu_set_reg(c->hv, HV_REG_CPSR, PSTATE_EL1H_MASKED));
            c->offset_dirty = true;
        }
        if (c->offset_dirty) {  // 時鐘偏移變了（開機、或 Mac 剛睡醒）
            c->offset_dirty = false;
            HV(hv_vcpu_set_vtimer_offset(c->hv, vtimer_offset));
        }
        pthread_mutex_unlock(&lock);

        HV(hv_vcpu_run(c->hv));
        handle_exit(c);
        sync_vtimer(c);

        pthread_mutex_lock(&lock);
    }
    return NULL;
}

void vcpus_create(void) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < vm.ncpus; i++) {
        vcpu_t *c = &vm.cpus[i];
        c->index = i;
        if (pthread_create(&c->thread, NULL, vcpu_thread, c)) die("建立 vCPU 執行緒失敗\n");
        // 一顆一顆建立，確保 vCPU 編號跟中斷控制器的重分配器順序一致
        while (!c->created) pthread_cond_wait(&cond, &lock);
    }
    pthread_mutex_unlock(&lock);
}

void vcpu_boot(uint64_t pc, uint64_t x0) {
    pthread_mutex_lock(&lock);
    vcpu_t *c = &vm.cpus[0];
    c->start_pc = pc;
    c->start_x0 = x0;  // 開機規範：X0 = 硬體清單位址
    c->start_pending = true;
    c->powered_on = true;
    pthread_mutex_unlock(&lock);
}

void vm_start(void) {
    pthread_mutex_lock(&lock);
    vtimer_offset = mach_absolute_time();  // 虛擬機的時鐘從 0 開始
    sleep_seen = mach_continuous_time() - mach_absolute_time();
    started = true;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock);
    pthread_t t;
    pthread_create(&t, NULL, sleep_watch_thread, NULL);
    pthread_detach(t);
}
