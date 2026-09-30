# 效能測試：同一份程式碼編成 macOS 版與 Linux 版
bench.linux: guest_main.c bench_core.h mkelf.py
	clang -target aarch64-linux-gnu -O2 -ffreestanding -fno-stack-protector -fno-pic \
	      -mno-outline-atomics -fno-asynchronous-unwind-tables -c guest_main.c -o bench.o
	python3 mkelf.py bench.o $@
	rm -f bench.o

# 快速版：每項只跑一次，給實驗用
benchq.linux: guest_main.c bench_core.h mkelf.py
	clang -target aarch64-linux-gnu -O2 -ffreestanding -fno-stack-protector -fno-pic -DREPEAT=1 \
	      -mno-outline-atomics -fno-asynchronous-unwind-tables -c guest_main.c -o benchq.o
	python3 mkelf.py benchq.o $@
	rm -f benchq.o

bench.mac: host_main.c bench_core.h
	clang -O2 -o $@ host_main.c

all: bench.linux benchq.linux bench.mac
.PHONY: all
