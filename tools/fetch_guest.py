#!/usr/bin/env python3
"""下載 LeapVM 需要的 Linux 檔案（都來自 Alpine Linux 官方站，約 20MB）：

  guest/vmlinuz-virt            Linux 核心（壓縮格式，LeapVM 會自己解開）
  guest/alpine/initramfs-virt   Alpine 的安裝模式（tools/install_alpine.py 用）
  guest/bin/busybox.static      測試用迷你系統的指令集

執行 make 時會自動呼叫，不用手動跑。
"""
import os
import re
import subprocess
import tarfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MIRROR = "https://dl-cdn.alpinelinux.org/alpine/v3.24"


# 用 macOS 內建的 curl 下載：python.org 版的 Python 常常沒裝 SSL 憑證，用 urllib 會失敗
def fetch(url, dest):
    if os.path.exists(dest):
        return
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    print(f"下載 {url}", flush=True)
    subprocess.run(["curl", "-fsSL", "--retry", "3", "-o", dest + ".part", url], check=True)
    os.replace(dest + ".part", dest)


def main():
    guest = os.path.join(ROOT, "guest")
    fetch(f"{MIRROR}/releases/aarch64/netboot/vmlinuz-virt", os.path.join(guest, "vmlinuz-virt"))
    fetch(f"{MIRROR}/releases/aarch64/netboot/initramfs-virt", os.path.join(guest, "alpine/initramfs-virt"))

    busybox = os.path.join(guest, "bin/busybox.static")
    if not os.path.exists(busybox):
        listing = subprocess.run(["curl", "-fsSL", f"{MIRROR}/main/aarch64/"], capture_output=True, text=True, check=True).stdout
        name = sorted(set(re.findall(r'busybox-static-[0-9][^"]*?\.apk', listing)))[-1]
        apk = os.path.join(guest, name)
        fetch(f"{MIRROR}/main/aarch64/{name}", apk)
        with tarfile.open(apk, "r:gz") as t:  # .apk 其實就是 tar.gz
            member = t.getmember("bin/busybox.static")
            member.name = "busybox.static"
            try:
                t.extract(member, os.path.join(guest, "bin"), filter="data")
            except TypeError:  # Python 3.11 以前沒有 filter 參數
                t.extract(member, os.path.join(guest, "bin"))
        os.chmod(busybox, 0o755)
    print("Linux 檔案就緒")


if __name__ == "__main__":
    main()
