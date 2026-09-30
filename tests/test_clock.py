#!/usr/bin/env python3
"""測試：Mac 睡醒之後，Linux 的時鐘要一次跳到正確的時間（用 clockjump 指令模擬睡了一小時）。"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from guestlib import DISK, KERNEL, boot_disk, cleanup, output_since, power_off, shell
from harness import check

if not (os.path.exists(KERNEL) and os.path.exists(DISK)):
    print("（略過：還沒安裝 Linux）")
    sys.exit(0)


def guest_clock(vm, tag):
    shell(vm, f"echo {tag}=$(date +%s),$(cut -d. -f1 /proc/uptime)", f"{tag}_DONE", timeout=15)
    m = re.search(rf"{tag}=(\d+),(\d+)", output_since(vm, f"echo {tag}="))
    return int(m.group(1)), int(m.group(2))


vm = boot_disk("clock")
try:
    wall0, up0 = guest_clock(vm, "T0")
    host0 = time.time()
    vm.ctl("clockjump 3600")  # 假裝 Mac 睡了一小時
    time.sleep(1)
    wall1, up1 = guest_clock(vm, "T1")
    elapsed = time.time() - host0
    print(f"撥時鐘前：Linux 時間 {wall0}、開機 {up0} 秒；撥 3600 秒後：{wall1}、開機 {up1} 秒")
    check(3595 <= (wall1 - wall0) - elapsed <= 3605, f"牆上時鐘只跳了 {wall1 - wall0} 秒")
    check(3595 <= (up1 - up0) - elapsed <= 3605, f"開機時間只跳了 {up1 - up0} 秒")
    # 跳完之後系統要正常：定時器、排程都還在動
    shell(vm, "sleep 1; echo alive=$(nproc)", "ALIVE_DONE", timeout=15)
    check("alive=4\r" in output_since(vm, "echo alive="), "撥時鐘後 Linux 不正常")
    power_off(vm)
    print("✓ 時鐘測試通過：撥快 3600 秒後 Linux 的時間與開機時間都正確跳過去，系統照常運作")
finally:
    vm.stop()
    cleanup()
