#!/usr/bin/env python3
"""長時間滿載：CPU、硬碟、網路、DNS、滑鼠鍵盤、畫面同時操。用法：soak.py [分鐘，預設 10]"""
import os
import re
import sys
import time
from common import boot_disk, check, cleanup, guest_stats, host_rss_mb, output_since, power_off, shell, start_http

minutes = float(sys.argv[1]) if len(sys.argv) > 1 else 10
serve = "/tmp/leapvm-soak"
os.makedirs(serve, exist_ok=True)
with open(f"{serve}/f.bin", "wb") as f:
    f.write(os.urandom(64 * 1024 * 1024))
srv = start_http(serve, 18333)

vm = boot_disk("soak", mem_mb=4096)
try:
    time.sleep(15)  # 等桌面起來
    vm.type("for i in 1 2 3 4; do (while :; do :; done &); done")   # CPU：4 個忙碌迴圈
    vm.type("(while :; do dd if=/dev/urandom of=/root/soak bs=1M count=32 2>/dev/null; sha256sum /root/soak > /root/soak.sum; sync; "
            "sha256sum -c /root/soak.sum >/dev/null 2>&1 || echo DISK_\"MISMATCH\"; done) &")  # 硬碟：寫、存、驗
    vm.type("(n=0; while :; do n=$((n+1)); echo $n > /tmp/netloops; wget -q -O /dev/null http://10.0.2.2:18333/f.bin || echo NET_\"FAIL\"; "
            "nslookup www.wikipedia.org 10.0.2.3 >/dev/null 2>&1 || echo DNS_\"FAIL\"; done) &")  # 網路 + DNS
    t_end = time.time() + minutes * 60
    rss_start = None
    rounds = 0
    while time.time() < t_end:
        for _ in range(60):  # 30 秒內不停送滑鼠、鍵盤事件、截圖
            vm.ctl(f"mouse {100 + rounds % 900} {100 + (rounds * 7) % 600}")
            vm.ctl("keys 42")  # Shift
            time.sleep(0.5)
        vm.ctl("screenshot /tmp/leapvm-soak/shot.png")
        rounds += 1
        rss = host_rss_mb(vm)
        if rss_start is None:
            rss_start = rss
        t0 = time.time()
        shell(vm, "echo load=$(cut -d' ' -f1-3 /proc/loadavg) kerr=$(dmesg | grep -ci -E 'BUG|Oops|WARNING|EXT4-fs error')", f"R{rounds}_DONE", timeout=30)
        resp = time.time() - t0
        line = [l for l in output_since(vm, "echo load=").splitlines() if l.startswith("load=")][0].strip()
        text = vm.text()
        bad = text.count("DISK_MISMATCH"), text.count("NET_FAIL"), text.count("DNS_FAIL")
        print(f"[{rounds * 0.5:4.1f} 分] 主機記憶體 {rss:5.0f} MB｜{line}｜回應 {resp:.1f} 秒｜硬碟錯 {bad[0]} 網路錯 {bad[1]} DNS 錯 {bad[2]}", flush=True)
        check(resp < 10, "虛擬機回應太慢")
        check("kerr=0" in line, "Linux 核心出現錯誤訊息")
    shell(vm, "kill $(jobs -p) 2>/dev/null; pkill -f 'while :'; rm -f /root/soak /root/soak.sum; echo netloops=$(cat /tmp/netloops)", "STOP_DONE", timeout=30)
    loops = int(re.search(r"netloops=(\d+)", output_since(vm, "echo netloops=")).group(1))
    time.sleep(1)
    for s in guest_stats(vm):
        print("   " + s.split("] ", 1)[1])
    rss_end = host_rss_mb(vm)
    text = vm.text()
    dns_fail = text.count("DNS_FAIL")
    print(f"   網路迴圈 {loops} 次（每次下載 64MB + 查一次 DNS）；下載失敗 {text.count('NET_FAIL')} 次、DNS 失敗 {dns_fail} 次")
    check(text.count("DISK_MISMATCH") == 0, "硬碟資料驗證失敗過")
    check(text.count("NET_FAIL") == 0, "網路下載失敗過")
    check(dns_fail <= max(2, loops // 1000), "DNS 失敗太多次")  # 上游 DNS 偶爾慢一下是正常的，超過千分之一才算問題
    check(rss_end - rss_start < 150, f"主機記憶體增加 {rss_end - rss_start:.0f} MB，可能有洩漏")
    power_off(vm)
    print(f"✓ 滿載 {minutes:g} 分鐘：沒有錯誤、回應正常、主機記憶體 {rss_start:.0f} → {rss_end:.0f} MB")
finally:
    vm.stop()
    cleanup()
    srv.shutdown()
    os.remove(f"{serve}/f.bin")
