#!/bin/bash
# LeapVM：雙擊就能在 Mac 上開一台 Linux 電腦
cd "$(dirname "$0")" || exit 1
make -s leapvm guest/vmlinuz-virt guest/alpine/initramfs-virt || { echo "編譯失敗"; read -r; exit 1; }

# 第一次使用：先把 Linux 安裝進虛擬硬碟（需要網路，大約 10～20 分鐘）
if [ ! -f guest/installed/vmlinuz-virt ]; then
    echo "第一次使用，要先把 Linux 安裝到虛擬硬碟。"
    python3 tools/install_alpine.py && python3 tools/setup_desktop.py || { echo "安裝失敗"; read -r; exit 1; }
fi

echo "LeapVM 開機中……會跳出一個視窗，Linux 桌面會自動登入。"
echo "關掉視窗、或在 Linux 裡按「登出 → 關機」就會結束。"
./leapvm -k guest/installed/vmlinuz-virt -i guest/installed/initramfs-virt \
    -c 4 -m 4096 --disk guest/disk.img --display \
    -a "console=tty1 console=ttyAMA0 root=/dev/vda3 rootfstype=ext4 modules=ext4 quiet"
