#!/usr/bin/env python3
"""測試：Linux 桌面可以用滑鼠和鍵盤操作（需要先安裝桌面：tools/install_alpine.py、tools/setup_desktop.py）。

  1. 開機，等 XFCE 桌面出現
  2. 滑鼠移到下方工具列的「終端機」圖示、點一下 → 終端機要開起來
  3. 用鍵盤在終端機裡打一行指令 → 檔案內容要跟打的一樣
另外透過序列埠（root）檢查結果，並把畫面存成 PNG。
"""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from guestlib import DISK, KERNEL, boot_disk, cleanup, output_since, power_off, shell
from harness import check

if not (os.path.exists(KERNEL) and os.path.exists(DISK)):
    print("（略過：還沒安裝 Linux 桌面）")
    sys.exit(0)

SHOTS = "/tmp/leapvm-shots"
os.makedirs(SHOTS, exist_ok=True)
t0 = time.time()
vm = boot_disk("desktop", mem_mb=4096)
try:
    # 等桌面程式都起來
    for _ in range(60):
        shell(vm, "pgrep -x xfdesktop >/dev/null && echo DESKTOP_UP || echo DESKTOP_NOT_YET", "POLL_DONE", timeout=10)
        if output_since(vm, "pgrep -x xfdesktop").count("DESKTOP_UP") >= 1 and "DESKTOP_UP\r" in output_since(vm, "pgrep -x xfdesktop"):
            break
        time.sleep(1)
    print(f"桌面出現：開機後 {time.time() - t0:.0f} 秒")
    time.sleep(3)
    vm.ctl(f"screenshot {SHOTS}/desktop.png")

    # 桌面剛出現時，工具列的圖示還在載入（會先顯示成齒輪），點了不一定有反應，所以沒開就隔幾秒再點
    vm.ctl("mouse 567 773")   # 下方工具列的終端機圖示
    opened = False
    for attempt in range(12):
        vm.ctl("click 0")
        for _ in range(5):
            time.sleep(1)
            shell(vm, "pgrep -x xfce4-terminal >/dev/null && echo TERM_OPEN || echo TERM_NOT_YET", "TPOLL_DONE", timeout=10)
            if "TERM_OPEN\r" in output_since(vm, "pgrep -x xfce4-terminal"):
                opened = True
                break
        if opened:
            break
    check(opened, "點了終端機圖示，但終端機沒有開起來")
    print(f"桌面可以操作：開機後 {time.time() - t0:.0f} 秒")
    print("✓ 滑鼠：點工具列圖示，終端機開起來了")

    time.sleep(2)
    vm.ctl("type echo Hello-LeapVM_123 > /tmp/typed.txt\\n")
    time.sleep(1)
    vm.ctl(f"screenshot {SHOTS}/typed.png")
    shell(vm, "cat /tmp/typed.txt", "TYPED_DONE", timeout=10)
    check("Hello-LeapVM_123" in output_since(vm, "cat /tmp/typed.txt"), "在終端機打的字沒有進到 Linux")
    print("✓ 鍵盤：在終端機打的字（含大寫和符號）正確送進 Linux")
    vm.ctl("type ls /\\n")
    time.sleep(1)
    vm.ctl(f"screenshot {SHOTS}/terminal.png")
    print(f"畫面存在 {SHOTS}/desktop.png、terminal.png")
    power_off(vm)
    print("✓ 電源鍵：Linux 收到後自己正常關機")
finally:
    vm.stop()
    cleanup()
