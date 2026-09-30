#!/usr/bin/env python3
"""測試：開機到命令列，4 顆 CPU 都有起來，可以打指令、正常關機。"""
from harness import BOOT, VM, check, cleanup

vm = VM("boot", BOOT + ["-c", "4"])
try:
    vm.wait_for(r"leapvm:~#")
    vm.type("echo cpus=$(nproc) mem=$(free -m | awk '/Mem:/{print $2}')")
    m = vm.wait_for(r"cpus=(\d+) mem=(\d+)")
    check(m.group(1) == "4", f"應該有 4 顆 CPU，實際 {m.group(1)}")
    check(int(m.group(2)) > 400, f"記憶體太少：{m.group(2)} MB")
    vm.type("dmesg | grep -ci -E 'BUG|Oops|rcu.*stall'")
    vm.wait_for(r"\n0\r?\n")
    vm.type("poweroff -f")
    vm.wait_for(r"Linux 要求關機")
    print(f"✓ 開機測試通過（{m.group(1)} 顆 CPU、{m.group(2)} MB 記憶體、正常關機）")
finally:
    vm.stop()
    cleanup()
