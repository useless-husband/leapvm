"""LeapVM 測試工具：在背景啟動虛擬機、對它打字、等它印出某段文字。"""
import os
import re
import shutil
import subprocess
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEAPVM = os.path.join(ROOT, "leapvm")
BOOT = ["-k", os.path.join(ROOT, "guest/vmlinuz-virt"), "-i", os.path.join(ROOT, "guest/initramfs.cpio")]
TMP = tempfile.mkdtemp(prefix="leapvm-test.", dir="/tmp")

# 在 Linux 裡每 0.1 秒數一個數字
COUNTER = "i=0; while true; do i=$((i+1)); echo tick $i; sleep 0.1; done &"


class VM:
    def __init__(self, name, args):
        os.makedirs(TMP, exist_ok=True)  # cleanup() 可能已經把資料夾刪掉了（一個測試開好幾台的時候）
        self.name = name
        self.sock = os.path.join(TMP, name + ".sock")
        self.out_path = os.path.join(TMP, name + ".out")
        self.out = open(self.out_path, "wb")
        self.proc = subprocess.Popen([LEAPVM, *args, "--ctl", self.sock],
                                     stdin=subprocess.PIPE, stdout=self.out, stderr=subprocess.STDOUT)

    def text(self):
        with open(self.out_path, "rb") as f:
            return f.read().decode("utf-8", "replace")

    def type(self, line):
        self.proc.stdin.write(line.encode() + b"\n")
        self.proc.stdin.flush()

    def ctl(self, command):
        return subprocess.run([LEAPVM, "--send", self.sock, command], capture_output=True, text=True).stdout

    def wait_for(self, pattern, timeout=20):
        deadline = time.time() + timeout
        while time.time() < deadline:
            m = re.search(pattern, self.text())
            if m:
                return m
            if self.proc.poll() is not None and not re.search(pattern, self.text()):
                break
            time.sleep(0.05)
        raise SystemExit(f"✗ [{self.name}] 等不到「{pattern}」\n--- 最後的輸出 ---\n{self.text()[-2000:]}")

    def ticks(self):
        return ticks_in(self.text())

    def log_lines(self, keyword):
        return [l.strip() for l in self.text().splitlines() if "[LeapVM]" in l and keyword in l]

    def stop(self):
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait()


def guest_output(*vms):
    """把好幾台（依搬移順序）的畫面接成一條，只留 Linux 印的內容。

    搬移可能剛好發生在 Linux 印一行字的中途：前半行在來源的畫面、後半行在目的端的畫面。
    所以要先拿掉 LeapVM 自己的訊息、再接起來，才看得到完整的每一行。
    """
    texts = [v if isinstance(v, str) else v.text() for v in vms]
    return "".join(re.sub(r"\[LeapVM\][^\n]*\n?", "", t) for t in texts)


def ticks_in(text):
    # 只算印完整的行（後面有換行）。畫面的最後一行可能還在印，例如「tick 31」只出現了「tick 3」
    return [int(n) for n in re.findall(r"tick (\d+)\r?\n", text)]


def check_continuous(*vms):
    """計數器從第一台到最後一台必須連續：不跳號、不重複。回傳全部的數字。"""
    ticks = ticks_in(guest_output(*vms))
    check(ticks, "沒有看到計數器")
    gaps = [(a, b) for a, b in zip(ticks, ticks[1:]) if b != a + 1]
    check(not gaps, f"計數器不連續：{gaps[:5]}")
    return ticks


def cleanup():
    shutil.rmtree(TMP, ignore_errors=True)


def check(cond, message):
    if not cond:
        raise SystemExit("✗ " + message)
