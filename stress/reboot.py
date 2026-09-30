#!/usr/bin/env python3
"""反覆開關機。用法：reboot.py [次數，預設 15]"""
import sys
import time
from common import boot_disk, check, cleanup, host_rss_mb, power_off

n = int(sys.argv[1]) if len(sys.argv) > 1 else 15
boots = []
for i in range(1, n + 1):
    t0 = time.time()
    vm = boot_disk(f"reboot{i}")
    try:
        boot = time.time() - t0
        vm.type("echo ok=$(cat /etc/hostname)")
        vm.wait_for(r"ok=leapvm", timeout=10)
        rss = host_rss_mb(vm)
        t1 = time.time()
        power_off(vm)
        vm.proc.wait(timeout=10)
        check(vm.proc.returncode == 0, f"第 {i} 次：結束碼 {vm.proc.returncode}")
        boots.append(boot)
        print(f"第 {i:2d} 次 ✓ 開機到登入 {boot:4.1f} 秒｜關機 {time.time() - t1:4.1f} 秒｜主機記憶體 {rss:.0f} MB")
    finally:
        vm.stop()
        cleanup()
print(f"✓ 連續 {n} 次開關機全部成功；開機時間 {min(boots):.1f}～{max(boots):.1f} 秒")
