#!/usr/bin/env python3
"""把 Alpine Linux 安裝進 LeapVM 的虛擬硬碟（guest/disk.img）。

流程：
  1. 用 Alpine 官方的網路開機檔案開一個「安裝模式」的 Linux（全部在記憶體裡）
  2. 用設定檔自動執行 setup-alpine，裝進虛擬硬碟 /dev/vda
  3. 把硬碟裡的核心和 initramfs 透過網路傳回 Mac（guest/installed/），
     之後 LeapVM 就用這兩個檔案從硬碟開機，版本保證跟硬碟裡的驅動程式一致

用法：tools/install_alpine.py [硬碟大小 GB，預設 16]
"""
import base64
import os
import socket
import sys
import threading
import time

from guestlib import ROOT, VM, cleanup, shell

REPO = "http://dl-cdn.alpinelinux.org/alpine/v3.24"
DISK = os.path.join(ROOT, "guest/disk.img")
OUT = os.path.join(ROOT, "guest/installed")

ANSWERS = f"""KEYMAPOPTS=none
HOSTNAMEOPTS=leapvm
DEVDOPTS=mdev
INTERFACESOPTS="auto lo
iface lo inet loopback

auto eth0
iface eth0 inet dhcp
"
DNSOPTS=none
TIMEZONEOPTS="Asia/Taipei"
PROXYOPTS=none
APKREPOSOPTS="{REPO}/main {REPO}/community"
USEROPTS=none
SSHDOPTS=none
NTPOPTS=none
DISKOPTS="-m sys /dev/vda"
LBUOPTS=none
APKCACHEOPTS=none
"""


def receive_files(port, names):
    """在 Mac 這邊等 Linux 把檔案傳過來（Linux 連 10.0.2.2 就是連到 Mac 的 127.0.0.1）。"""
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(1)
    got = {}

    def run():
        for name in names:
            conn, _ = srv.accept()
            data = bytearray()
            while chunk := conn.recv(1 << 20):
                data.extend(chunk)
            conn.close()
            got[name] = bytes(data)
        srv.close()

    t = threading.Thread(target=run, daemon=True)
    t.start()
    return t, got


def main():
    size_gb = int(sys.argv[1]) if len(sys.argv) > 1 else 16
    if os.path.exists(DISK) and os.path.getsize(DISK) > 0 and os.stat(DISK).st_blocks > 0:
        answer = input(f"{DISK} 已經有資料，要清掉重新安裝嗎？（輸入 yes 確認）")
        if answer != "yes":
            return
    with open(DISK, "wb") as f:
        f.truncate(size_gb * 1024**3)  # 稀疏檔：只佔用實際寫進去的空間
    os.makedirs(OUT, exist_ok=True)

    print(f"開啟安裝模式（{size_gb} GB 硬碟）……")
    vm = VM("install", ["-k", f"{ROOT}/guest/vmlinuz-virt", "-i", f"{ROOT}/guest/alpine/initramfs-virt",
                        "-c", "4", "-m", "2048", "--disk", DISK,
                        "-a", f"console=ttyAMA0 ip=dhcp alpine_repo={REPO}/main "
                              f"modloop={REPO}/releases/aarch64/netboot/modloop-virt"])
    try:
        vm.wait_for(r"login:", timeout=180)
        vm.type("root")
        vm.wait_for(r"localhost:~# ", timeout=30)
        time.sleep(2)  # 讓命令列先問完終端機的游標位置，再開始打字

        print("執行 setup-alpine，安裝到虛擬硬碟（需要下載套件，大約 1～3 分鐘）……")
        b64 = base64.b64encode(ANSWERS.encode()).decode()
        shell(vm, f"echo {b64} | base64 -d > /tmp/answers", "ANSWERS_WRITTEN")
        shell(vm, "ERASE_DISKS=/dev/vda BOOTLOADER=none setup-alpine -e -f /tmp/answers 2>&1 | tail -15",
              "SETUP_FINISHED", timeout=900)
        shell(vm, "blkid; mount /dev/vda3 /mnt && mount /dev/vda1 /mnt/boot && ls -l /mnt/boot && "
                  "ls /mnt/lib/modules", "INSPECTED")
        t = vm.text()
        print(t[t.rindex("blkid"):][:1500])

        print("把核心和 initramfs 傳回 Mac……")
        thread, got = receive_files(18123, ["vmlinuz-virt", "initramfs-virt"])
        for name in ["vmlinuz-virt", "initramfs-virt"]:
            shell(vm, f"nc 10.0.2.2 18123 < /mnt/boot/{name}", f"SENT_{name.replace('-', '_').upper()}")
        thread.join(timeout=60)
        for name, data in got.items():
            with open(os.path.join(OUT, name), "wb") as f:
                f.write(data)
            print(f"  {name}：{len(data) / 1024 / 1024:.1f} MB")
        shell(vm, "umount /mnt/boot /mnt; sync", "UNMOUNTED")
        vm.type("poweroff")
        vm.wait_for(r"Linux 要求關機", timeout=60)
        print("✓ 安裝完成")
    finally:
        vm.stop()
        cleanup()


if __name__ == "__main__":
    main()
