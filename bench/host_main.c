// macOS 版：同一份測試直接在 Mac 上跑
#include "bench_core.h"
#include <stdio.h>
#include <sys/mman.h>

int main(void) {
    uint64_t *buf = mmap(NULL, BUF_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    uint64_t best[3], checksum = 0;
    run_all(buf, best, &checksum);
    printf("BENCH %llu %llu %llu %llu %llu\n", best[0], best[1], best[2], tick_freq(), checksum);
    return 0;
}
