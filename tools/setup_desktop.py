#!/usr/bin/env python3
"""在虛擬硬碟裡的 Alpine 安裝 XFCE 桌面，並設定開機自動登入。

安裝內容：XFCE 桌面、Firefox、中文字型、使用者 leap（密碼 leap，可以用 doas 取得管理員權限）。
需要先執行 tools/install_alpine.py。下載量大約 1GB，視網路速度需要幾分鐘到十幾分鐘。
"""
import base64
import time
from guestlib import boot_disk, cleanup, output_since, shell, shutdown

LIGHTDM = """[Seat:*]
autologin-user=leap
autologin-user-timeout=0
autologin-session=xfce
"""

STEPS = [
    ("更新套件清單", "apk update 2>&1 | tail -1", 120),
    ("安裝 XFCE 桌面與 Firefox（最久的一步）", "setup-desktop xfce 2>&1 | tail -5", 3600),
    ("安裝中文字型", "apk add font-noto-cjk 2>&1 | tail -1", 1200),
    ("建立使用者 leap", "adduser -D -g LeapVM leap 2>&1; echo leap:leap | chpasswd; "
                       "for g in wheel video input audio netdev; do addgroup leap $g; done; "
                       "apk add doas 2>&1 | tail -1; echo 'permit persist :wheel' > /etc/doas.d/wheel.conf", 300),
    ("設定開機自動登入", f"echo {base64.b64encode(LIGHTDM.encode()).decode()} | base64 -d > /etc/lightdm/lightdm.conf.d/50-leapvm.conf 2>/dev/null || "
                        f"(mkdir -p /etc/lightdm/lightdm.conf.d && echo {base64.b64encode(LIGHTDM.encode()).decode()} | base64 -d > /etc/lightdm/lightdm.conf.d/50-leapvm.conf); "
                        "addgroup -S autologin 2>/dev/null; addgroup leap autologin", 60),
    ("按電源鍵就關機（關 LeapVM 視窗時會用到）", "rc-update add acpid default", 60),
    ("確認", "rc-update show default; df -h / | tail -1", 60),
]


def main():
    vm = boot_disk("desktop")
    try:
        for i, (title, cmd, timeout) in enumerate(STEPS, 1):
            print(f"[{i}/{len(STEPS)}] {title}……", flush=True)
            t0 = time.time()
            shell(vm, cmd, f"STEP{i}_DONE", timeout=timeout)
            out = output_since(vm, cmd[:30]).splitlines()[1:-2]
            for line in out[-8:]:
                print("    " + line.strip())
            print(f"    （{time.time() - t0:.0f} 秒）", flush=True)
        shutdown(vm)
        print("✓ 桌面安裝完成")
    finally:
        vm.stop()
        cleanup()


if __name__ == "__main__":
    main()
