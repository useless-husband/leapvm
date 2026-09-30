CC      = clang
# 最低支援 macOS 15：中斷控制器 API（hv_gic_*）從這一版開始提供
CFLAGS  = -O2 -g -Wall -Wextra -std=gnu11 -mmacosx-version-min=15.0
SLIRP  ?= $(shell brew --prefix libslirp 2>/dev/null || echo /opt/homebrew/opt/libslirp)
CFLAGS += -I$(SLIRP)/include
LDLIBS  = -framework Hypervisor -framework Cocoa -framework QuartzCore -framework ImageIO \
          -L$(SLIRP)/lib -lslirp -lresolv -lz
SRC     = $(wildcard src/*.c) $(wildcard src/*.m)

leapvm: $(SRC) src/vm.h src/virtio.h entitlements.plist
	$(CC) $(CFLAGS) -fobjc-arc -o $@ $(SRC) $(LDLIBS)
	codesign --entitlements entitlements.plist --force -s - $@

# Linux 核心與 BusyBox 從 Alpine 官方站下載（第一次 make 時）
guest/vmlinuz-virt guest/bin/busybox.static guest/alpine/initramfs-virt:
	python3 tools/fetch_guest.py

guest/initramfs.cpio: guest/mkinitramfs.py guest/init guest/bin/busybox.static guest/extra/bench guest/extra/benchq guest/extra/demo
	python3 guest/mkinitramfs.py

guest/extra/bench: bench/guest_main.c bench/bench_core.h bench/mkelf.py
	$(MAKE) -C bench bench.linux
	mkdir -p guest/extra && cp bench/bench.linux $@

guest/extra/benchq: bench/guest_main.c bench/bench_core.h bench/mkelf.py
	$(MAKE) -C bench benchq.linux
	mkdir -p guest/extra && cp bench/benchq.linux $@

guest/extra/demo: guest/demo/demo.c bench/mkelf.py
	mkdir -p guest/extra
	clang -target aarch64-linux-gnu -O2 -ffreestanding -fno-stack-protector -fno-pic \
	      -mno-outline-atomics -fno-asynchronous-unwind-tables -c guest/demo/demo.c -o guest/demo/demo.o
	python3 bench/mkelf.py guest/demo/demo.o $@
	rm -f guest/demo/demo.o

# 測試用的迷你 Linux（只有命令列）；完整桌面請看 README 的安裝步驟
run: leapvm guest/vmlinuz-virt guest/initramfs.cpio
	./leapvm -k guest/vmlinuz-virt -i guest/initramfs.cpio --display

clean:
	rm -rf leapvm leapvm.dSYM guest/initramfs.cpio guest/extra bench/bench.mac bench/*.linux

.PHONY: run clean
