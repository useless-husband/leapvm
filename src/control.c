// LeapVM：控制通道
//
// 虛擬機執行時，另一個終端機可以透過 Unix socket 下指令：
//   leapvm --send <socket> "screenshot 畫面.png"
// 自動化測試也是靠這個通道操作。
#include "vm.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <stdio.h>
#include <sys/un.h>
#include <unistd.h>

static char sock_path[104];

// 自動化測試用：把文字換成按鍵（美式鍵盤）。回傳 Linux 按鍵代碼，*shift 表示要不要按住 Shift
static int char_to_key(char c, bool *shift) {
    static const char *rows[] = {"1234567890-=", "qwertyuiop[]", "asdfghjkl;'", "zxcvbnm,./"};
    static const char *shifted[] = {"!@#$%^&*()_+", "QWERTYUIOP{}", "ASDFGHJKL:\"", "ZXCVBNM<>?"};
    static const int first[] = {2, 16, 30, 44};  // 每一排第一個鍵的代碼
    *shift = false;
    for (int r = 0; r < 4; r++) {
        const char *p = strchr(rows[r], c);
        if (p && c) return first[r] + (int)(p - rows[r]);
        p = strchr(shifted[r], c);
        if (p && c) { *shift = true; return first[r] + (int)(p - shifted[r]); }
    }
    switch (c) {
    case ' ': return 57;
    case '\n': return 28;
    case '`': return 41;
    case '~': *shift = true; return 41;
    case '\\': return 43;
    case '|': *shift = true; return 43;
    }
    return 0;
}

// 文字裡的 \n 代表 Enter、\t 代表 Tab（控制指令以換行結尾，所以不能直接放換行字元）
static void type_text(const char *s) {
    for (; *s; s++) {
        bool shift;
        int k;
        if (s[0] == '\\' && s[1] == 'n') { k = 28; shift = false; s++; }
        else if (s[0] == '\\' && s[1] == 't') { k = 15; shift = false; s++; }
        else k = char_to_key(*s, &shift);
        if (!k) continue;
        if (shift) input_key(42, true);
        input_key(k, true);
        input_key(k, false);
        if (shift) input_key(42, false);
        usleep(8000);  // 太快的話，有些程式會漏字
    }
}

// 按下一組按鍵再依相反順序放開，例如 "29+56+20" = Ctrl+Alt+T
static void press_combo(char *combo) {
    int keys[8], n = 0;
    for (char *t = strtok(combo, "+"); t && n < 8; t = strtok(NULL, "+")) keys[n++] = atoi(t);
    for (int i = 0; i < n; i++) input_key(keys[i], true);
    usleep(30000);
    for (int i = n - 1; i >= 0; i--) input_key(keys[i], false);
}

static void handle_command(char *line, int fd) {
    line[strcspn(line, "\r\n")] = 0;
    char *rest = line + strcspn(line, " ");
    if (*rest) *rest++ = 0;  // line = 指令，rest = 後面全部
    char *cmd = line;
    const char *reply = "ok\n";

    if (!*cmd) reply = "error: 空指令\n";
    else if (!strcmp(cmd, "stats")) print_stats();
    else if (!strcmp(cmd, "screenshot") && *rest) { if (!display_save_png(rest)) reply = "error: 沒有螢幕或存檔失敗\n"; }
    else if (!strcmp(cmd, "type")) type_text(rest);
    else if (!strcmp(cmd, "keys") && *rest) press_combo(rest);
    else if (!strcmp(cmd, "mouse") && *rest && vm.fb_width) {  // mouse <x> <y>（像素）
        double x = 0, y = 0;
        sscanf(rest, "%lf %lf", &x, &y);
        input_pointer_move(x / vm.fb_width, y / vm.fb_height);
    } else if (!strcmp(cmd, "click")) {  // click [0 左 | 1 右 | 2 中]
        int b = *rest ? atoi(rest) : 0;
        input_pointer_button(b, true);
        usleep(50000);
        input_pointer_button(b, false);
    }
    else if (!strcmp(cmd, "clockjump") && *rest) vm_clock_forward((uint64_t)atoll(rest));  // 模擬 Mac 睡了 N 秒
    else if (!strcmp(cmd, "powerbutton")) { input_key(116, true); input_key(116, false); }  // KEY_POWER
    else if (!strcmp(cmd, "quit")) { (void)!write(fd, reply, strlen(reply)); vm_exit(0); }
    else reply = "error: 可用指令：stats | screenshot <檔案.png> | type <文字，\\n 是 Enter> | keys <代碼+代碼> | "
                 "mouse <x> <y> | click [按鈕] | clockjump <秒> | powerbutton | quit\n";
    (void)!write(fd, reply, strlen(reply));
}

static void *control_thread(void *arg) {
    int lfd = (int)(intptr_t)arg;
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) continue;
        char line[512];
        ssize_t n = read(fd, line, sizeof line - 1);
        if (n > 0) {
            line[n] = 0;
            handle_command(line, fd);
        }
        close(fd);
    }
    return NULL;
}

static void remove_socket(void) { unlink(sock_path); }

void control_start(const char *path) {
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof addr.sun_path) die("控制通道的路徑太長\n");
    strcpy(addr.sun_path, path);
    strcpy(sock_path, path);
    unlink(path);
    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&addr, sizeof addr) || listen(lfd, 4))
        die("無法建立控制通道 %s：%s\n", path, strerror(errno));
    atexit(remove_socket);
    pthread_t t;
    pthread_create(&t, NULL, control_thread, (void *)(intptr_t)lfd);
}

// 用戶端：送一行指令、印出回覆
int control_send(const char *path, const char *command) {
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, path, sizeof addr.sun_path - 1);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof addr)) {
        fprintf(stderr, "連不上控制通道 %s：%s\n", path, strerror(errno));
        return 1;
    }
    (void)!write(fd, command, strlen(command));
    char buf[512];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) fwrite(buf, 1, (size_t)n, stdout);
    close(fd);
    return 0;
}
