#!/usr/bin/env python3
"""測試：LeapVM 收到結束訊號（SIGTERM，例如關掉終端機視窗）時，要先讓 Linux 正常關機，不能直接死掉。"""
import os
import signal
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from guestlib import DISK, KERNEL, boot_disk, cleanup
from harness import check

if not (os.path.exists(KERNEL) and os.path.exists(DISK)):
    print("（略過：還沒安裝 Linux）")
    sys.exit(0)

vm = boot_disk("signal")
try:
    vm.type("touch /root/marker-before-shutdown; sync")
    time.sleep(1)
    vm.proc.send_signal(signal.SIGTERM)
    vm.wait_for(r"按下電源鍵", timeout=5)
    vm.wait_for(r"Linux 要求關機", timeout=60)
    vm.proc.wait(timeout=5)
    check(vm.proc.returncode == 0, f"結束碼應該是 0，實際 {vm.proc.returncode}")
    check("Power down" in vm.text() or "reboot: Power down" in vm.text() or "要求關機" in vm.text(), "沒有看到 Linux 關機")
    print("✓ 收到 SIGTERM：按下電源鍵 → Linux 自己關機 → LeapVM 正常結束")
finally:
    vm.stop()
    cleanup()
