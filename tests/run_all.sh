#!/bin/bash
# 執行全部測試
cd "$(dirname "$0")/.." && make -s leapvm guest/initramfs.cpio || exit 1
fail=0
run() { echo; echo "━━ $* ━━"; python3 "$@" || fail=1; }
run tests/test_boot.py
run tests/test_display.py
run tests/test_desktop.py   # 以下三個需要先安裝 Linux 桌面，沒安裝會自動略過
run tests/test_signal.py
run tests/test_reset.py
run tests/test_clock.py
echo
[ $fail = 0 ] && echo "全部測試通過" || { echo "有測試失敗"; exit 1; }
