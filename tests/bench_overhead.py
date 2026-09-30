#!/usr/bin/env python3
"""量測虛擬化的效能損失：同一支測速程式，在 Mac 上直接跑 vs 在 LeapVM 裡跑。

三項測試：
  運算      純整數運算，不碰記憶體
  連續記憶體 依序讀寫 256MB
  隨機記憶體 在 256MB 裡隨機跳著讀寫（最考驗虛擬化的位址轉換）
"""
import os
import re
import subprocess
from harness import BOOT, ROOT, VM, cleanup

NAMES = ["運算", "連續記憶體", "隨機記憶體"]


def parse(text):
    m = re.search(r"BENCH (\d+) (\d+) (\d+) (\d+) (\d+)", text)
    t = [int(m.group(i)) for i in range(1, 4)]
    freq = int(m.group(4))
    return [x / freq for x in t], int(m.group(5))


def main():
    rounds = int(os.environ.get("ROUNDS", "3"))
    host_runs, guest_runs = [], []
    vm = VM("bench", BOOT + ["-c", "1", "-m", "1024"])
    try:
        vm.wait_for(r"leapvm:~#")
        for r in range(rounds):
            # 輪流跑，避免 Mac 當時的狀態（溫度、背景程式）只影響其中一邊
            out = subprocess.run([os.path.join(ROOT, "bench/bench.mac")], capture_output=True, text=True).stdout
            host_runs.append(parse(out))
            before = len(re.findall(r"BENCH ", vm.text()))
            vm.type("bench")
            vm.wait_for(r"(?s)" + r"BENCH \d+ \d+ \d+ \d+ \d+.*" * (before + 1), timeout=300)
            guest_runs.append(parse(vm.text().split("BENCH ")[-1].join(["BENCH ", ""]) if False else "BENCH " + vm.text().split("BENCH ")[-1]))
    finally:
        vm.stop()
        cleanup()

    assert all(h[1] == g[1] for h, g in zip(host_runs, guest_runs)), "兩邊的計算結果不一致"
    print(f"（每項取 {rounds} 輪、每輪 5 次中最快的一次；兩邊計算結果一致）")
    print(f"{'項目':<10}{'Mac 直接跑':>12}{'LeapVM 裡':>12}{'損失':>10}")
    for i, name in enumerate(NAMES):
        h = min(r[0][i] for r in host_runs)
        g = min(r[0][i] for r in guest_runs)
        print(f"{name:<10}{h:>11.3f}s{g:>11.3f}s{(g / h - 1) * 100:>+9.1f}%")


if __name__ == "__main__":
    main()
