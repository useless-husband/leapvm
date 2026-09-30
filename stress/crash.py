#!/usr/bin/env python3
"""突然斷電：檔案寫到一半直接 kill -9，重開機後之前存好的檔案要完整、檔案系統要能自己修復。"""
import time
from common import boot_disk, check, cleanup, output_since, power_off, shell

vm = boot_disk("crash1")
try:
    # 第一批：50 個檔案，算好 SHA-256 並確實寫進硬碟
    shell(vm, "mkdir -p /root/batch1 && cd /root/batch1 && for i in $(seq 50); do head -c 200000 /dev/urandom > f$i; done && "
              "sha256sum f* > /root/batch1.sum && sync && echo batch1=ok", "B1_DONE", timeout=120)
    check("batch1=ok" in output_since(vm, "batch1.sum"), "第一批檔案沒寫成功")
    # 第二批：不停寫，寫到一半把 LeapVM 直接殺掉（等於拔電源）
    vm.type("mkdir -p /root/batch2; i=0; while :; do i=$((i+1)); head -c 1000000 /dev/urandom > /root/batch2/f$i; done &")
    time.sleep(3)
    vm.proc.kill()
    vm.proc.wait()
    print("已在寫檔途中直接殺掉 LeapVM（模擬拔電源）")
finally:
    vm.stop()
    cleanup()

vm = boot_disk("crash2")
try:
    shell(vm, "cd /root/batch1 && sha256sum -c /root/batch1.sum 2>&1 | grep -c ': OK'", "CHK_DONE", timeout=60)
    ok = int([l for l in output_since(vm, "grep -c ': OK'").splitlines() if l.strip().isdigit()][0])
    check(ok == 50, f"第一批只有 {ok}/50 個檔案完整")
    shell(vm, "echo recov=$(dmesg | grep -c -i 'recovery complete'); echo exterr=$(dmesg | grep -c 'EXT4-fs error'); "
              "echo ro=$(grep ' / ' /proc/mounts | grep -c ' ro,'); ls /root/batch2 | wc -l", "FS_DONE", timeout=30)
    out = output_since(vm, "echo recov=")
    check("exterr=0" in out, "檔案系統有錯誤（EXT4-fs error）")
    check("ro=0" in out, "根目錄被掛成唯讀，代表檔案系統壞了")
    print(f"重開機後：第一批 50/50 完整、日誌修復訊息 {'有' if 'recov=1' in out or 'recov=2' in out else '無'}、沒有檔案系統錯誤、根目錄可寫")
    shell(vm, "rm -rf /root/batch1 /root/batch2 /root/batch1.sum; sync", "CLEAN_DONE", timeout=60)
    power_off(vm)
    print("✓ 斷電測試通過")
finally:
    vm.stop()
    cleanup()
