"""壓力測試共用。"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, os.path.join(HERE, "..", "tests"))
from guestlib import DISK, KERNEL, boot_disk, cleanup, guest_stats, host_rss_mb, output_since, power_off, shell  # noqa
from harness import check  # noqa
from bench_io import start_http  # noqa

if not (os.path.exists(KERNEL) and os.path.exists(DISK)):
    print("（略過：還沒安裝 Linux）")
    sys.exit(0)
