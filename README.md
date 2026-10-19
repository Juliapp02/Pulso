<h1 align="center">pulso</h1>

<p align="center">
  <a href="https://en.wikipedia.org/wiki/C_(programming_language)"><img src="https://img.shields.io/badge/Language-C-00599C?logo=c&logoColor=white" alt="Language: C" /></a>
  <a href="https://ebpf.io"><img src="https://img.shields.io/badge/Kernel-eBPF-ECA918?logo=linux&logoColor=white" alt="Kernel: eBPF" /></a>
  <a href="https://www.raylib.com/"><img src="https://img.shields.io/badge/UI-raylib-black" alt="UI: raylib" /></a>
  <a href="https://github.com/libbpf/libbpf"><img src="https://img.shields.io/badge/Library-libbpf_1.x-orange" alt="Library: libbpf" /></a>
  <img src="https://img.shields.io/badge/Platform-Linux-FCC624?logo=linux&logoColor=black" alt="Platform: Linux" />
</p>

<p align="center">
  <b>pulso</b> is a btop-style system monitor for Linux, written in C. Metrics are collected <i>inside the kernel</i> via eBPF tracepoints and rendered in real time with raylib — no /proc polling, no sampling gaps.
</p>

---

## 💡 Motivation

Most system monitors (htop, btop…) gather CPU, network and disk metrics by reading `/proc` and `/sys` at fixed intervals. That works well in practice, but it has a fundamental limitation: anything that happens *between* two polls is invisible. Short-lived processes, quick bursts of disk I/O, brief network spikes — all averaged out or simply missed.

eBPF tracepoints fire on every scheduler event, every packet and every completed block request. That means:

- **CPU time is exact**, not inferred from `/proc/stat` snapshots.
- **Per-process accounting** tracks every context switch, so no process silently steals CPU between samples.
- **Network and disk rates** are computed from the actual bytes the kernel processed, not from counters read with a delay.

pulso was built to explore what a monitor looks like when the data source lives in the kernel itself.

---

## ✨ Metrics

| Metric | Source |
| --- | --- |
| CPU per core | `tp_btf/sched_switch` — time the core spent outside the idle task |
| CPU per process | `tp_btf/sched_switch` — accumulated runtime per TGID in an LRU map |
| Network | `tp_btf/net_dev_xmit` and `tp_btf/netif_receive_skb`, loopback excluded |
| Disk | `tp_btf/block_rq_complete`, split by reads and writes |
| Memory | `/proc/meminfo` (`MemTotal − MemAvailable`) |

The daemon also accounts for the task *currently running* on each core at sampling time, so a process that never yields the CPU does not appear at 0 % until its next context switch.

---

## 🏗️ Architecture

pulso is split into two separate binaries:

- **`pulsod`** loads the eBPF programs, reads their maps and `.bss` counters once per second, and publishes a binary snapshot on `/run/pulso.sock`. It is the only component that requires root.
- **`pulso`** is the graphical interface. It connects to the socket, maintains a rolling history buffer and draws everything with raylib.

```
 kernel                       pulsod (root)                    pulso
┌───────────────────┐       ┌──────────────────────┐        ┌───────────────┐
│ sched_switch      │       │ reads maps and .bss  │  unix  │ graphs        │
│ net_dev_xmit      │ ────▶ │ computes percentages │ ─────▶ │ cores         │
│ netif_receive_skb │       │ and rates per second │ socket │ processes     │
│ block_rq_complete │       └──────────────────────┘        └───────────────┘
└───────────────────┘
```

Keeping them separate means the GUI runs as a normal user, and multiple clients can read from the same daemon simultaneously.

Each connection to `/run/pulso.sock` receives a `struct snapshot` (defined in `src/pulso.h`) as raw binary and the daemon closes the connection immediately. The `tick` field increments with each sample so the client can detect and skip repeated snapshots.

---

## 📁 Structure

```
src/
├── probe.bpf.c   eBPF programs (tracepoints)
├── daemon.c      pulsod: probe loading, sampling loop and socket server
├── gui.c         pulso: raylib-based interface
└── pulso.h       shared types between all three
```

---

## ⚙️ Requirements & Build

- Linux with BTF (`/sys/kernel/btf/vmlinux`) and `tp_btf` support
- `clang`, `bpftool`, `libbpf` 1.x, `raylib` 5 or later, `make`

On Arch Linux:

```sh
sudo pacman -S clang bpf libbpf raylib make
```

Then just:

```sh
make
```

`make` generates `build/vmlinux.h` from the running kernel's BTF, compiles the eBPF program, generates its skeleton with bpftool, and links both `pulsod` and `pulso`.

---

## 🚀 Usage

```sh
sudo ./pulsod &
./pulso
```

`pulso` waits until the daemon is available, so the startup order does not matter. To stop the daemon: `Ctrl+C` — it cleans up the socket on exit.

---

## ⚠️ Limitations

- The socket is created with `0666` permissions, so any local user can read the process list (the same information already exposed by `/proc`).
- CPU usage per process groups all its threads, so it can exceed 100 % — same behaviour as `top`.
- At most the 64 most CPU-intensive processes are reported; up to 256 cores are supported.
- On very small windows (e.g. in a tiling window manager) text labels may overlap.
