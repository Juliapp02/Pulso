CFLAGS ?= -O2 -Wall -Wextra
BPF_CFLAGS = -O2 -g -target bpf -Wno-missing-declarations

all: pulsod pulso

build/vmlinux.h:
	mkdir -p build
	bpftool btf dump file /sys/kernel/btf/vmlinux format c > $@

build/probe.bpf.o: src/probe.bpf.c src/pulso.h build/vmlinux.h
	clang $(BPF_CFLAGS) -Ibuild -c $< -o $@

build/probe.skel.h: build/probe.bpf.o
	bpftool gen skeleton $< > $@

pulsod: src/daemon.c src/pulso.h build/probe.skel.h
	$(CC) $(CFLAGS) -Ibuild $< -o $@ $$(pkg-config --libs libbpf)

pulso: src/gui.c src/pulso.h
	$(CC) $(CFLAGS) $< -o $@ $$(pkg-config --cflags --libs raylib) -lm

clean:
	rm -rf build pulsod pulso

.PHONY: all clean
