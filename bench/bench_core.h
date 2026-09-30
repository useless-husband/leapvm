// 效能測試的核心：同一份程式碼分別編成 macOS 版和 Linux 版，比較兩邊的速度。
// 計時直接讀 CPU 的計數器（CNTVCT_EL0）。Mac 上和虛擬機裡的計數頻率可能不同，
// 所以兩邊都會一併回報頻率（CNTFRQ_EL0），各自換算成秒再比較。
#pragma once
#include <stddef.h>
#include <stdint.h>

#define INLINE static inline __attribute__((always_inline))

INLINE uint64_t tick_freq(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

INLINE uint64_t ticks(void) {
    uint64_t v;
    __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(v) :: "memory");
    return v;
}

// 1. 純運算：整數雜湊，不碰記憶體
INLINE uint64_t cpu_work(uint64_t n) {
    uint64_t x = 0x9E3779B97F4A7C15ULL, acc = 0;
    for (uint64_t i = 0; i < n; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        acc += x * 0xff51afd7ed558ccdULL;
        __asm__ volatile("" : "+r"(acc));
    }
    return acc;
}

// 2. 連續讀寫大塊記憶體：測記憶體頻寬
INLINE uint64_t seq_work(uint64_t *buf, size_t words, int passes) {
    uint64_t sum = 0;
    for (int p = 0; p < passes; p++)
        for (size_t i = 0; i < words; i++) {
            buf[i] += (uint64_t)p + i;
            sum += buf[i];
            __asm__ volatile("" : "+r"(sum));
        }
    return sum;
}

// 3. 隨機跳著存取：每次都落在不同的分頁，專門考驗「位址轉換」。
//    虛擬機的位址要轉兩次（Linux 一次、Hypervisor 一次），這是虛擬化最吃虧的情況。
INLINE uint64_t rand_work(uint64_t *buf, size_t words, uint64_t n) {
    uint64_t idx = 1, sum = 0;
    for (uint64_t i = 0; i < n; i++) {
        idx = idx * 6364136223846793005ULL + 1442695040888963407ULL;
        size_t j = (idx >> 17) % words;
        buf[j] += i;
        sum += buf[j];
        __asm__ volatile("" : "+r"(sum));
    }
    return sum;
}

#define CPU_N      400000000ULL
#define BUF_BYTES  (256ULL << 20)
#define SEQ_PASSES 8
#define RAND_N     30000000ULL
#ifndef REPEAT
#define REPEAT     5
#endif

// 跑三項測試各 REPEAT 次，取最快的一次（排除被其他程式干擾的情況）
INLINE void run_all(uint64_t *buf, uint64_t best[3], uint64_t *checksum) {
    for (int k = 0; k < 3; k++) best[k] = ~0ULL;
    for (size_t i = 0; i < BUF_BYTES / 8; i++) buf[i] = i;  // 先碰過每一頁，排除第一次配置的成本
    for (int r = 0; r < REPEAT; r++) {
        uint64_t t0 = ticks();
        *checksum += cpu_work(CPU_N);
        uint64_t t1 = ticks();
        *checksum += seq_work(buf, BUF_BYTES / 8, SEQ_PASSES);
        uint64_t t2 = ticks();
        *checksum += rand_work(buf, BUF_BYTES / 8, RAND_N);
        uint64_t t3 = ticks();
        if (t1 - t0 < best[0]) best[0] = t1 - t0;
        if (t2 - t1 < best[1]) best[1] = t2 - t1;
        if (t3 - t2 < best[2]) best[2] = t3 - t2;
    }
}
