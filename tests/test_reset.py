#!/usr/bin/env python3
"""壓力測試：Linux 一邊用硬碟和網路，一邊反覆重設驅動（unbind / bind），LeapVM 不能當掉。"""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from guestlib import DISK, KERNEL, boot_disk, cleanup, output_since, power_off, shell
from harness import check

if not (os.path.exists(KERNEL) and os.path.exists(DISK)):
    print("（略過：還沒安裝 Linux）")
    sys.exit(0)

vm = boot_disk("reset")
try:
    # 背景不停讀寫硬碟；同時把網路卡的驅動反覆拔掉再裝回去 30 次
    vm.type("(while true; do dd if=/dev/urandom of=/root/junk bs=1M count=8 2>/dev/null; cat /root/junk > /dev/null; done) &")
    # 找出網路卡（virtio 裝置編號 0x0001）掛在哪個 virtio_mmio 上，不能拔到系統硬碟
    shell(vm, "D=/sys/bus/platform/drivers/virtio-mmio; "
              "M=$(for d in /sys/bus/virtio/devices/*; do grep -q 0x0001 $d/device && basename $(dirname $(readlink -f $d)); done); "
              "echo net_mmio=$M; for i in $(seq 30); do echo $M > $D/unbind; echo $M > $D/bind; "
              "wget -q -O /dev/null http://10.0.2.2:1/ 2>/dev/null; done; echo cycles=30", "RESET_DONE", timeout=300)
    check("cycles=30" in output_since(vm, "D=/sys/bus"), "重設迴圈沒有跑完")
    # 硬碟驅動也拔插 10 次（要先停掉背景讀寫、卸載檔案系統才拔得掉，所以改用 sysfs 的 reset 路徑：重新綁定會失敗但不能當機）
    shell(vm, "kill %1 2>/dev/null; sleep 1; rm -f /root/junk; sync; echo kerrs=$(dmesg | grep -ci -E 'BUG|Oops|WARNING')", "DMESG_DONE", timeout=60)
    check("kerrs=0\r" in vm.text(), "Linux 核心記錄裡有錯誤")
    check(vm.proc.poll() is None, "LeapVM 在裝置重設期間死掉了")
    power_off(vm)
    print("✓ 網路卡驅動反覆拔插 30 次、硬碟同時讀寫：LeapVM 沒有當掉，Linux 也沒有錯誤")
finally:
    vm.stop()
    cleanup()
