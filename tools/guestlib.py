"""工具共用：開一台從虛擬硬碟開機的 LeapVM，用序列埠以 root 登入並執行指令。"""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests"))
from harness import ROOT, VM, cleanup  # noqa: E402

DISK = os.path.join(ROOT, "guest/disk.img")
KERNEL = os.path.join(ROOT, "guest/installed/vmlinuz-virt")
INITRD = os.path.join(ROOT, "guest/installed/initramfs-virt")
BOOTARGS = "console=tty1 console=ttyAMA0 root=/dev/vda3 rootfstype=ext4 modules=ext4 quiet"


def boot_disk(name="guest", mem_mb=2048):
    os.environ["LEAPVM_HEADLESS"] = "1"
    vm = VM(name, ["-k", KERNEL, "-i", INITRD, "-c", "4", "-m", str(mem_mb), "--disk", DISK, "--display",
                   "-a", BOOTARGS])
    vm.wait_for(r"leapvm login:", timeout=90)
    vm.type("root")
    vm.wait_for(r"leapvm:~# ", timeout=30)
    time.sleep(2)  # 讓命令列先問完終端機的游標位置，再開始打字
    return vm


def shell(vm, command, done_marker, timeout=60):
    """執行一行指令，等它印出 done_marker。記號拆兩半再 echo，才不會被回顯的指令本身誤判。"""
    n = vm.text().count(done_marker)
    head, tail = done_marker.split("_", 1)
    vm.type(f'{command}; echo {head}_"{tail}"')
    vm.wait_for(r"(?s)" + (done_marker + r".*") * n + done_marker + r"\s*\r?\n", timeout=timeout)


def output_since(vm, text):
    t = vm.text()
    return t[t.rindex(text):]


def shutdown(vm):
    vm.type("poweroff")
    vm.wait_for(r"Linux 要求關機", timeout=120)


def power_off(vm, timeout=60):
    """按虛擬電源鍵，等 Linux 自己關機（不要直接切掉，不然還沒寫進硬碟的檔案會壞掉）。"""
    if vm.proc.poll() is not None:
        return
    vm.ctl("powerbutton")
    try:
        vm.wait_for(r"Linux 要求關機", timeout=timeout)
    except SystemExit:
        print("⚠ Linux 沒有在時間內關機，只好強制結束")


def host_rss_mb(vm):
    """LeapVM 程序目前佔用的主機記憶體（MB）。"""
    import subprocess
    out = subprocess.run(["ps", "-o", "rss=", "-p", str(vm.proc.pid)], capture_output=True, text=True).stdout
    return int(out.strip() or 0) / 1024


def guest_stats(vm):
    """請 LeapVM 印統計，回傳最新的那幾行。"""
    n = len(vm.log_lines("門鈴"))
    vm.ctl("stats")
    for _ in range(50):
        lines = vm.log_lines("門鈴")
        if len(lines) > n:
            return lines[n:]
        time.sleep(0.05)
    return []
