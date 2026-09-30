#!/usr/bin/env python3
"""把 busybox 和 init 打包成 Linux 的 initramfs（cpio newc 格式）。

用 Python 自己寫 cpio，是為了在 macOS 上不用 root 權限也能放進 /dev/console 這種裝置檔。
"""
import os
import time

HERE = os.path.dirname(os.path.abspath(__file__))
out = bytearray()
ino = 1


def entry(name, mode, data=b"", rdev=(0, 0)):
    global ino
    fields = [ino, mode, 0, 0, 1, int(time.time()), len(data), 0, 0, rdev[0], rdev[1], len(name) + 1, 0]
    out.extend(b"070701" + "".join("%08X" % f for f in fields).encode() + name.encode() + b"\0")
    out.extend(b"\0" * (-len(out) % 4))
    out.extend(data)
    out.extend(b"\0" * (-len(out) % 4))
    ino += 1


for d in ["bin", "dev", "proc", "sys", "tmp", "root", "sbin", "usr", "usr/bin", "usr/sbin", "etc"]:
    entry(d, 0o040755)
entry("dev/console", 0o020600, rdev=(5, 1))
entry("bin/busybox", 0o100755, open(os.path.join(HERE, "bin/busybox.static"), "rb").read())
entry("init", 0o100755, open(os.path.join(HERE, "init"), "rb").read())
entry("etc/passwd", 0o100644, b"root:x:0:0:root:/root:/bin/sh\n")
# guest/extra/ 裡的檔案放進 /bin（例如效能測試程式）
extra = os.path.join(HERE, "extra")
if os.path.isdir(extra):
    for name in sorted(os.listdir(extra)):
        entry("bin/" + name, 0o100755, open(os.path.join(extra, name), "rb").read())
entry("TRAILER!!!", 0)

path = os.path.join(HERE, "initramfs.cpio")
open(path, "wb").write(out)
print(f"initramfs：{path}（{len(out) // 1024} KB）")
