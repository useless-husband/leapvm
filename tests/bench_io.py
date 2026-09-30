#!/usr/bin/env python3
"""量測硬碟與網路的效能損失：同樣的事情在 Linux 裡做、在 Mac 上直接做，比較速度。

硬碟：寫入 1GB（寫完確實存進硬碟），再讀回來
網路：從 Mac 上的網頁伺服器下載 512MB 的檔案；另外從網路下載一個檔案
需要先安裝 Linux（tools/install_alpine.py）。
"""
import http.server
import os
import re
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from guestlib import DISK, boot_disk, cleanup, output_since, power_off, shell, ROOT

MB = 1024 * 1024
TMPDIR = os.path.join(ROOT, "guest")  # 跟虛擬硬碟檔放在同一顆 SSD 上，比較才公平


def host_disk():
    path = os.path.join(TMPDIR, "bench.tmp")
    data = os.urandom(MB)
    t = time.time()
    with open(path, "wb") as f:
        for _ in range(1024):
            f.write(data)
        f.flush()
        os.fsync(f.fileno())
    w = 1024 / (time.time() - t)
    t = time.time()
    with open(path, "rb") as f:
        while f.read(MB):
            pass
    r = 1024 / (time.time() - t)
    os.remove(path)
    return w, r


class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a):
        pass


def start_http(directory, port):
    handler = lambda *a, **k: Quiet(*a, directory=directory, **k)  # noqa: E731
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def seconds(text):
    m = re.search(r"real\s+(?:(\d+)m\s*)?([\d.]+)s?", text)
    return int(m.group(1) or 0) * 60 + float(m.group(2))


def main():
    rows = []
    hw, hr = host_disk()

    # 網頁伺服器上放一個 512MB 的檔案，Linux 連 10.0.2.2 就是連到 Mac 自己
    serve = os.path.join(TMPDIR, "serve")
    os.makedirs(serve, exist_ok=True)
    with open(os.path.join(serve, "big.bin"), "wb") as f:
        f.write(os.urandom(512 * MB))
    srv = start_http(serve, 18222)
    t = time.time()
    subprocess.run(["curl", "-s", "-o", "/dev/null", "http://127.0.0.1:18222/big.bin"], check=True)
    host_net = 512 / (time.time() - t)
    url = "http://dl-cdn.alpinelinux.org/alpine/v3.24/community/aarch64/APKINDEX.tar.gz"
    t = time.time()
    size = int(subprocess.run(["curl", "-s", "-o", "/dev/null", "-w", "%{size_download}", url],
                              capture_output=True, text=True).stdout)
    host_inet = size / MB / (time.time() - t)

    vm = boot_disk("bench", mem_mb=2048)
    try:
        shell(vm, "sync; time dd if=/dev/urandom of=/root/rand.bin bs=1M count=64 2>/dev/null", "PREP_DONE", 60)
        # 用 64MB 亂數重複寫成 1GB，避免測到產生亂數的速度
        shell(vm, "sync; echo 3 > /proc/sys/vm/drop_caches; time sh -c 'for i in $(seq 16); do cat /root/rand.bin; done "
                  "> /root/bench.bin; sync'", "DW_DONE", 300)
        gw = 1024 / seconds(output_since(vm, "time sh -c"))
        shell(vm, "echo 3 > /proc/sys/vm/drop_caches; time cat /root/bench.bin > /dev/null", "DR_DONE", 300)
        gr = 1024 / seconds(output_since(vm, "time cat /root/bench.bin"))
        shell(vm, "rm -f /root/bench.bin /root/rand.bin; sync", "RM_DONE", 60)

        shell(vm, "time wget -q -O /dev/null http://10.0.2.2:18222/big.bin", "NET_DONE", 300)
        gn = 512 / seconds(output_since(vm, "time wget -q -O /dev/null http://10.0.2.2"))
        shell(vm, f"time wget -q -O /dev/null {url}", "INET_DONE", 300)
        gi = size / MB / seconds(output_since(vm, "time wget -q -O /dev/null http://dl"))
        power_off(vm)
    finally:
        vm.stop()
        cleanup()
        srv.shutdown()
        os.remove(os.path.join(serve, "big.bin"))
        os.rmdir(serve)

    rows = [("硬碟寫入 1GB（含存進硬碟）", hw, gw), ("硬碟讀取 1GB", hr, gr),
            ("從 Mac 下載 512MB（本機網路）", host_net, gn), (f"從網路下載 {size / MB:.1f}MB", host_inet, gi)]
    print(f"{'項目':<24}{'Mac 直接':>12}{'LeapVM 裡':>12}{'比例':>9}")
    for name, h, g in rows:
        print(f"{name:<24}{h:>9.0f} MB/s{g:>9.0f} MB/s{g / h * 100:>8.0f}%")


if __name__ == "__main__":
    main()
