// LeapVM：從零打造的 Apple Silicon 虛擬機，讓 Linux 在 Mac 上跑
//
// 開機流程：
//   1. 向 macOS 要一台空的虛擬機，配一塊記憶體給它
//   2. 建立中斷控制器（GICv3，由 Hypervisor.framework 提供）
//   3. 把 Linux 核心、initramfs、硬體清單放進虛擬機記憶體
//   4. 建立虛擬 CPU，從核心的第一行指令開始執行
//   5. 每當 Linux 做了硬體無法直接處理的事（碰到裝置、呼叫 PSCI、閒置等待），
//      CPU 就會暫停並「離開」虛擬機（VM exit），由我們模擬完再讓它繼續
#include "virtio.h"
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include <zlib.h>

vm_t vm;

// ── 終端機 ──
static struct termios orig_tio;
static bool tio_saved;

void restore_terminal(void) {
    if (tio_saved) tcsetattr(STDIN_FILENO, TCSANOW, &orig_tio);
    tio_saved = false;
}

_Noreturn void vm_exit(int code) {
    restore_terminal();
    exit(code);
}

_Noreturn void die(const char *fmt, ...) {
    restore_terminal();
    va_list ap;
    va_start(ap, fmt);
    fputs("\n[LeapVM] 錯誤：", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

void print_stats(void) {
    vcpu_stats_t t = {0};
    for (int i = 0; i < vm.ncpus; i++) {
        const vcpu_stats_t *s = &vm.cpus[i].stats;
        t.mmio_read += s->mmio_read; t.mmio_write += s->mmio_write; t.hvc += s->hvc;
        t.wfi += s->wfi; t.sysreg += s->sysreg; t.vtimer += s->vtimer;
    }
    LOG("VM exit 統計：裝置讀 %llu｜裝置寫 %llu｜PSCI %llu｜閒置 %llu｜系統暫存器 %llu｜計時器 %llu\r\n",
        t.mmio_read, t.mmio_write, t.hvc, t.wfi, t.sysreg, t.vtimer);
    virtio_print_stats();
}

// ── 建立虛擬機：記憶體 + 中斷控制器 ──
static void vm_create(uint64_t ram_bytes, int ncpus) {
    if (ncpus < 1 || ncpus > MAX_VCPUS) die("CPU 數量必須在 1 到 %d 之間\n", MAX_VCPUS);
    if (ram_bytes < 64 * MiB) die("記憶體至少要 64 MB\n");
    vm.ncpus = ncpus;
    vm.ram_size = ram_bytes;

    HV(hv_vm_create(NULL));
    vm.ram = mmap(NULL, vm.ram_size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (vm.ram == MAP_FAILED) die("配置 %llu MB 記憶體失敗\n", ram_bytes / MiB);
    HV(hv_vm_map(vm.ram, RAM_BASE, vm.ram_size, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));

    // 中斷控制器必須在建立 vCPU 之前建立
    uint32_t spi_count;
    HV(hv_gic_get_redistributor_region_size(&vm.redist_region_size));
    HV(hv_gic_get_spi_interrupt_range(&vm.spi_base, &spi_count));
    hv_gic_config_t gic = hv_gic_config_create();
    HV(hv_gic_config_set_distributor_base(gic, GIC_DIST_BASE));
    HV(hv_gic_config_set_redistributor_base(gic, GIC_REDIST_BASE));
    HV(hv_gic_create(gic));
}

static uint8_t *load_file(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("打不開 %s：%s\n", path, strerror(errno));
    struct stat sb;
    fstat(fd, &sb);
    uint8_t *buf = malloc((size_t)sb.st_size);
    if (!buf) die("記憶體不足\n");
    for (size_t got = 0; got < (size_t)sb.st_size;) {  // read() 不保證一次讀完
        ssize_t n = read(fd, buf + got, (size_t)sb.st_size - got);
        if (n <= 0) die("讀取 %s 失敗\n", path);
        got += (size_t)n;
    }
    close(fd);
    *len = (size_t)sb.st_size;
    return buf;
}

// 發行版的核心（vmlinuz）通常是壓縮過的「EFI zboot」格式：開頭寫著 "zimg"，
// 後面接一段 gzip 壓縮的真正核心。這裡把它解開，拿到可以直接執行的 Image。
static uint8_t *unpack_zboot(uint8_t *file, size_t *len) {
    if (*len < 64 || memcmp(file + 4, "zimg", 4)) return file;  // 不是 zboot：原樣使用
    uint32_t off, size;
    memcpy(&off, file + 8, 4);
    memcpy(&size, file + 12, 4);
    if (memcmp(file + 24, "gzip", 4) || (uint64_t)off + size > *len) die("不支援的壓縮核心格式\n");
    size_t cap = 128 * MiB;
    uint8_t *out = malloc(cap);
    z_stream z = {.next_in = file + off, .avail_in = size, .next_out = out, .avail_out = (uInt)cap};
    if (!out || inflateInit2(&z, 16 + MAX_WBITS) != Z_OK || inflate(&z, Z_FINISH) != Z_STREAM_END)
        die("解壓縮核心失敗\n");
    *len = z.total_out;
    inflateEnd(&z);
    free(file);
    return out;
}

// 把核心、initramfs、硬體清單放進記憶體，設定開機 CPU 的起點
static void boot_linux(const char *kernel_path, const char *initrd_path, const char *bootargs) {
    size_t klen;
    uint8_t *kernel = unpack_zboot(load_file(kernel_path, &klen), &klen);
    if (klen < 64 || memcmp(kernel + 0x38, "ARM\x64", 4)) die("%s 不是 arm64 核心 Image\n", kernel_path);
    uint64_t text_offset, image_size;
    memcpy(&text_offset, kernel + 8, 8);
    memcpy(&image_size, kernel + 16, 8);
    if (image_size < klen) image_size = klen;
    // arm64 開機規範：核心放在 2MB 對齊的位址 + text_offset
    uint64_t kernel_addr = RAM_BASE + text_offset;
    uint64_t next = ALIGN_UP(kernel_addr + image_size, 2 * MiB);
    if (next - RAM_BASE > vm_usable_ram()) die("記憶體不夠放核心\n");
    memcpy(vm.ram + (kernel_addr - RAM_BASE), kernel, klen);
    free(kernel);

    uint64_t initrd_start = 0, initrd_end = 0;
    if (initrd_path) {
        size_t ilen;
        uint8_t *initrd = load_file(initrd_path, &ilen);
        initrd_start = next;
        initrd_end = initrd_start + ilen;
        if (initrd_end - RAM_BASE > vm_usable_ram()) die("記憶體不夠放 initramfs\n");
        memcpy(vm.ram + (initrd_start - RAM_BASE), initrd, ilen);
        free(initrd);
        next = ALIGN_UP(initrd_end, 2 * MiB);
    }

    uint64_t dtb_addr = next;
    if (dtb_addr + 2 * MiB - RAM_BASE > vm_usable_ram()) die("記憶體不夠放硬體清單\n");
    // 告訴 Linux 裡的程式螢幕在哪裡（核心沒有內建顯示驅動，程式要自己去寫那塊記憶體）
    char args[1024];
    if (vm.fb_width)
        snprintf(args, sizeof args, "%s leapvm.fb=0x%llx,%u,%u", bootargs, RAM_BASE + vm.fb_offset, vm.fb_width, vm.fb_height);
    else
        snprintf(args, sizeof args, "%s", bootargs);
    size_t dtb_len = build_fdt(vm.ram + (dtb_addr - RAM_BASE), 2 * MiB, args, initrd_start, initrd_end, vm.ncpus);
    LOG("%d 顆 CPU｜記憶體 %llu MB｜核心 @%#llx（%llu MB）｜initramfs @%#llx｜硬體清單 @%#llx（%zu bytes）\n",
        vm.ncpus, vm.ram_size / MiB, kernel_addr, image_size / MiB, initrd_start, dtb_addr, dtb_len);

    vcpu_boot(kernel_addr, dtb_addr);
}

// ── 訊號：被要求結束時，先讓 Linux 好好關機 ──
// 關掉終端機視窗（SIGHUP）、系統登出、被 kill（SIGTERM）時，如果直接死掉，
// Linux 還沒寫進硬碟的資料會不見。所以改成按虛擬電源鍵、等 Linux 自己關機；
// 30 秒沒關、或再收到一次訊號，才強制結束。
static bool have_power_button;

static void *signal_thread(void *arg) {
    sigset_t *set = arg;
    int sig;
    if (sigwait(set, &sig) != 0) return NULL;
    if (!have_power_button) {
        LOG("收到結束訊號，虛擬機結束\r\n");
        vm_exit(0);
    }
    LOG("收到結束訊號：按下電源鍵，等 Linux 關機……（再收到一次就強制結束）\r\n");
    input_key(116, true);  // KEY_POWER
    input_key(116, false);
    // macOS 沒有 sigtimedwait：每 0.1 秒看一次有沒有再收到訊號，最多等 30 秒
    for (int i = 0; i < 300; i++) {
        usleep(100 * 1000);
        sigset_t pending;
        sigpending(&pending);
        if (sigismember(&pending, SIGTERM) || sigismember(&pending, SIGHUP) || sigismember(&pending, SIGINT)) {
            LOG("再次收到結束訊號，強制結束\r\n");
            vm_exit(0);
        }
    }
    LOG("Linux 30 秒內沒有關機，強制結束\r\n");
    vm_exit(0);
}

static void install_signal_handling(void) {
    static sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, NULL);  // 之後建立的執行緒都會繼承這個設定
    pthread_t t;
    pthread_create(&t, NULL, signal_thread, &set);
    pthread_detach(t);
}

// ── 鍵盤 ──
// 處理一個按鍵。Ctrl-A 是 LeapVM 指令的開頭，其他按鍵照樣送給 Linux。
// 終端機和視窗各有一個 esc 狀態，互不干擾。
void console_key(uint8_t c, bool *esc) {
    if (*esc) {
        *esc = false;
        switch (c) {
        case 'x': LOG("使用者結束虛擬機\r\n"); print_stats(); vm_exit(0);
        case 'i': print_stats(); return;
        case 1: break;  // 連按兩次 Ctrl-A：送一個 Ctrl-A 給 Linux
        default: return;
        }
    } else if (c == 1) {
        *esc = true;
        return;
    }
    uart_input(c);
}

static void console_loop(void) {
    bool esc = false;
    uint8_t c;
    while (read(STDIN_FILENO, &c, 1) == 1) {
        if (!tio_saved && c == 1) { uart_input(c); continue; }  // 不是真的終端機（例如測試程式）：Ctrl-A 照送
        console_key(c, &esc);
    }
    for (;;) pause();  // 沒有鍵盤（例如在背景執行）：這條執行緒就一直待著
}

static void usage(const char *prog) {
    fprintf(stderr,
        "LeapVM — 從零打造的 Apple Silicon 虛擬機\n\n"
        "用法：%s -k <核心 Image> [-i <initramfs>] [-c <CPU 數>] [-m <記憶體 MB>] [-a <開機參數>]\n\n"
        "其他選項：\n"
        "  --disk <檔案>       虛擬硬碟（一個一般的檔案，Linux 看到的是 /dev/vda）\n"
        "  --no-net            不接網路\n"
        "  --display           開一個視窗顯示虛擬機的螢幕（1280×800），附虛擬鍵盤和滑鼠\n"
        "  --ctl <socket>      開啟控制通道\n"
        "  --send <socket> <指令>  對執行中的虛擬機下指令\n\n"
        "執行中的按鍵：Ctrl-A x 結束｜Ctrl-A i 統計\n",
        prog);
    exit(2);
}

static struct {
    const char *kernel_path, *initrd_path, *ctl_path, *bootargs, *disk_path;
    int ncpus;
    uint64_t mem_mb;
    bool display, no_net;
} opt = {
    .bootargs = "console=ttyAMA0 earlycon=pl011,mmio32,0x0c000000 quiet",
    .ncpus = 4,
    .mem_mb = 512,
};

// 虛擬機的一生：建立 → 開機 → 等鍵盤輸入
static void *vm_main(void *arg) {
    (void)arg;
    vm_create(opt.mem_mb * MiB, opt.ncpus);
    if (opt.display) {
        vm.fb_width = FB_WIDTH;
        vm.fb_height = FB_HEIGHT;
        vm.fb_offset = vm.ram_size - ALIGN_UP((uint64_t)FB_WIDTH * FB_HEIGHT * 4, 2 * MiB);
    }
    vcpus_create();
    uart_init();
    rtc_init();
    if (opt.disk_path) virtio_blk_init(opt.disk_path);
    if (!opt.no_net) virtio_net_init();
    if (opt.display) {
        virtio_input_init();
        have_power_button = true;
    }
    boot_linux(opt.kernel_path, opt.initrd_path, opt.bootargs);
    if (opt.ctl_path) control_start(opt.ctl_path);

    // 終端機切成原始模式，按鍵直接送進虛擬機
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &orig_tio) == 0) {
        struct termios raw = orig_tio;
        cfmakeraw(&raw);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        tio_saved = true;
        atexit(restore_terminal);
        LOG("按鍵：Ctrl-A x 結束｜Ctrl-A i 統計\r\n");
    }

    vm_start();
    display_attach();
    console_loop();
    return NULL;
}

int main(int argc, char **argv) {
    enum { OPT_CTL = 256, OPT_SEND, OPT_DISPLAY, OPT_DISK, OPT_NO_NET };
    static const struct option longopts[] = {
        {"ctl", required_argument, 0, OPT_CTL},
        {"disk", required_argument, 0, OPT_DISK},
        {"no-net", no_argument, 0, OPT_NO_NET},
        {"send", required_argument, 0, OPT_SEND},
        {"display", no_argument, 0, OPT_DISPLAY},
        {0, 0, 0, 0},
    };
    int o;
    while ((o = getopt_long(argc, argv, "k:i:m:c:a:", longopts, NULL)) != -1) {
        switch (o) {
        case 'k': opt.kernel_path = optarg; break;
        case 'i': opt.initrd_path = optarg; break;
        case 'm': opt.mem_mb = strtoull(optarg, NULL, 10); break;
        case 'c': opt.ncpus = atoi(optarg); break;
        case 'a': opt.bootargs = optarg; break;
        case OPT_CTL: opt.ctl_path = optarg; break;
        case OPT_DISPLAY: opt.display = true; break;
        case OPT_DISK: opt.disk_path = optarg; break;
        case OPT_NO_NET: opt.no_net = true; break;
        case OPT_SEND:
            if (optind >= argc) usage(argv[0]);
            return control_send(optarg, argv[optind]);
        default: usage(argv[0]);
        }
    }
    if (!opt.kernel_path) usage(argv[0]);
    signal(SIGPIPE, SIG_IGN);
    install_signal_handling();

    // 要開視窗的話，主執行緒交給 macOS 的視窗系統，虛擬機在另一條執行緒跑。
    // LEAPVM_HEADLESS：有螢幕但不開視窗（自動化測試用）
    if (opt.display && !getenv("LEAPVM_HEADLESS")) display_run(vm_main);
    vm_main(NULL);
    return 0;
}
