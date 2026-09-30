#!/usr/bin/env python3
"""測試：有螢幕的時候，畫面會更新（用 guest/demo 的測試動畫當作畫面內容）。"""
import os
import time

os.environ["LEAPVM_HEADLESS"] = "1"
from harness import BOOT, TMP, VM, check, cleanup

vm = VM("display", BOOT + ["-c", "4", "--display"])
try:
    vm.wait_for(r"leapvm:~#")
    time.sleep(1)
    vm.type("demo </dev/null >/dev/null 2>&1 &")
    time.sleep(1.5)
    vm.ctl(f"screenshot {TMP}/a.png")
    time.sleep(0.5)
    vm.ctl(f"screenshot {TMP}/b.png")
    a, b = (open(f"{TMP}/{n}.png", "rb").read() for n in "ab")
    check(len(a) > 1000 and a != b, "畫面沒有更新")
    print("✓ 畫面測試通過（螢幕有內容、而且會更新）")
finally:
    vm.stop()
    cleanup()
