#!/usr/bin/env python3
"""DNS 風暴：同時發 200 個查詢，之後一般查詢還要正常、等待中的查詢要歸零。"""
import time
from common import boot_disk, check, cleanup, guest_stats, host_rss_mb, output_since, power_off, shell

DOMAINS = ["www.google.com", "www.wikipedia.org", "www.apple.com", "www.github.com", "www.cloudflare.com",
           "www.microsoft.com", "www.youtube.com", "www.amazon.com", "www.mozilla.org", "www.python.org",
           "dl-cdn.alpinelinux.org", "www.debian.org", "kernel.org", "www.gnu.org", "www.ntu.edu.tw",
           "www.gov.tw", "tw.yahoo.com", "www.pchome.com.tw", "www.ptt.cc", "www.nvidia.com"]
vm = boot_disk("dns")
try:
    rss0 = host_rss_mb(vm)
    # 指令不能太長（終端機一行最多 4096 字元），所以 20 個網址重複 10 輪在 Linux 裡展開
    shell(vm, f"D='{' '.join(DOMAINS)}'; for r in $(seq 10); do for d in $D; do "
              "(nslookup $d 10.0.2.3 >/dev/null 2>&1 && echo hit) & done; done | grep -c hit", "STORM_DONE", timeout=120)
    out = output_since(vm, "grep -c hit")
    hits = int([l for l in out.splitlines() if l.strip().isdigit()][0])
    print(f"200 個同時查詢：{hits} 個成功")
    check(hits >= 190, "太多查詢失敗")
    shell(vm, "nslookup www.wikipedia.org 10.0.2.3 | grep -c Address", "AFTER_DONE", timeout=30)
    check(int([l for l in output_since(vm, "grep -c Address").splitlines() if l.strip().isdigit()][0]) >= 2, "風暴之後一般查詢失敗")
    time.sleep(3)
    stats = guest_stats(vm)
    net = [s for s in stats if "網路卡" in s][0]
    pending = int(net.split("DNS 等待中 ")[1].split()[0])
    check(pending == 0, f"風暴後還有 {pending} 個查詢卡著沒清掉")
    rss1 = host_rss_mb(vm)
    print(f"主機記憶體：{rss0:.0f} → {rss1:.0f} MB；{net.split('｜', 1)[1]}")
    check(rss1 - rss0 < 50, "記憶體用量異常增加")
    power_off(vm)
    print("✓ DNS 風暴測試通過")
finally:
    vm.stop()
    cleanup()
