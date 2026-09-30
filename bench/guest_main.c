// Linux 版：不用 C 函式庫，直接用系統呼叫（mmap、write、exit）
#include "bench_core.h"

INLINE long sys6(long n, long a, long b, long c, long d, long e, long f) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
    return x0;
}

INLINE int put_num(char *out, int k, uint64_t v) {
    char tmp[24];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) out[k++] = tmp[--n];
    return k;
}

void _start(void) {
    // mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
    uint64_t *buf = (uint64_t *)sys6(222, 0, (long)BUF_BYTES, 3, 0x22, -1, 0);
    uint64_t best[3], checksum = 0;
    run_all(buf, best, &checksum);

    char out[160];
    int k = 0;
    out[k++] = 'B'; out[k++] = 'E'; out[k++] = 'N'; out[k++] = 'C'; out[k++] = 'H';
    for (int i = 0; i < 3; i++) { out[k++] = ' '; k = put_num(out, k, best[i]); }
    out[k++] = ' ';
    k = put_num(out, k, tick_freq());
    out[k++] = ' ';
    k = put_num(out, k, checksum);
    out[k++] = '\n';
    sys6(64, 1, (long)out, k, 0, 0, 0);  // write
    sys6(93, 0, 0, 0, 0, 0, 0);          // exit
    __builtin_unreachable();
}
