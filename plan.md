# mona-rpzero — AArch64 Kernel Plan (QEMU → Raspberry Pi Zero 2 W)

Date: 2025-12-25

## Long-term goal

Build a **tiny, educational AArch64 kernel** that:

1. Boots **first in QEMU** (using `qemu-system-aarch64 -M raspi3b` as the "Pi Zero 2 W-ish" target).
2. Boots **later on real hardware** (**Raspberry Pi Zero 2 W**, BCM2710A1).
3. Provides a **Linux-like syscall ABI** (AArch64 Linux calling convention + syscall numbers) so that **certain small Linux AArch64 ELF binaries can run unmodified**.

Reference implementation for concepts and scope exists in:

- `archive/reference-x86/kernel/` — an x86-64 kernel that boots into an interactive shell and implements a minimal Linux syscall surface.

This document mirrors that approach, but adapted for AArch64 + Raspberry Pi.

## Constraints / design stance

- **QEMU-first, Pi-later**: early work optimizes for fast debug cycles.
- **Compatibility surface, not "Linux"**: implement only the syscalls we need, with Linux-compatible numbers/semantics where feasible.
- **Start with static binaries**: initial compatibility targets should be **statically linked** (e.g. musl `-static`) to avoid the dynamic linker and many extra syscalls.
- **Keep it debuggable**: early serial logging, deterministic failure modes, and simple memory layout.

## What “Linux-like ABI” means (AArch64)

### Syscall calling convention

Linux AArch64 uses `svc #0` with:

- syscall number in `x8`
- arguments in `x0..x5`
- return value in `x0`
- errors as negative values in `x0` (e.g. `-ENOENT`), mirroring Linux

Kernel requirements:

- Exception vector for synchronous exceptions from EL0
- A syscall dispatcher keyed by Linux syscall numbers

### Executable format

- Load **ELF64** (`ET_EXEC` and later `ET_DYN`/PIE)
- Set up user stack + `argc/argv/envp` + minimal auxv
- Enter EL0 at `e_entry` with correct SP

## Compatibility scope (phased)

The intent is to reach a point where a small set of “boring” Linux AArch64 programs can run:

- `true`, `false`, `echo`, `cat`, `ls` (eventually), and a minimal `sh`

To reduce complexity, the **first runnable milestone** should use:

- serial console only (stdin/stdout/stderr)
- initramfs-only filesystem (CPIO `newc`), read-only initially
- single-process first; then `fork/exec/wait` to support a shell

## Boot strategy

We need two boot paths that share as much kernel code as possible:

### Path A: QEMU (`raspi3b`)

- QEMU loads the kernel directly via `-kernel` and a DTB via `-dtb`.
- The kernel image format is typically a **raw binary** (`kernel8.img`) linked to the expected load address.
- Primary debug I/O is PL011 UART (`ttyAMA*` in Linux terms) exposed on the QEMU serial console.

### Path B: Real Raspberry Pi Zero 2 W

- The Pi boots via **GPU firmware** reading `kernel8.img` from the FAT boot partition.
- DTB is provided by firmware (or can be supplied explicitly).
- Start level may be EL2/EL1 depending on firmware settings; the kernel must handle and normalize this.

Design choice:

- Keep the kernel as a **raw image** (`kernel8.img`) for both environments.
- Always consume a **Device Tree** for memory + peripherals discovery.

## Architecture sketch (same spirit as the x86 reference)

- `arch/aarch64/`: exception vectors, EL transitions, MMU setup, timer, UART
- `sys/`: syscall dispatch + compatibility implementations
- `proc/`: tasks, scheduler, `fork/exec/wait`
- `fs/`: initramfs (CPIO newc) + minimal VFS + pipes
- `elf/`: ELF loader

## Phased roadmap (deliverables + acceptance tests)

### Phase 0 — Serial “Hello world” + halt

Status: DONE (2025-12-25)

Deliverables:

- Minimal AArch64 entry + stack
- PL011 UART init + `printk`
- Parse DTB pointer (passed by QEMU/firmware) and print machine model

Acceptance:

- `make run` prints a banner and halts cleanly in QEMU.

Notes:

- Implemented in `kernel-aarch64/` and validated via `make run-aarch64`.
- DTB parsing prints `/model` and first RAM range from `/memory*/reg`.

### Phase 1 — Exceptions + EL normalization

Status: DONE (2025-12-25)

Deliverables:

- Vector table installed
- Basic handlers for sync/irq/fiq/SError (dump ESR/ELR/FAR)
- Determine current exception level and drop to EL1 (if starting higher)

Acceptance:

- Intentional fault produces a readable dump on serial, no silent reset.

Notes:

- EL2→EL1 drop implemented (when applicable), VBAR_EL1 installed early.
- Exception dump includes `ESR_EL1`, `ELR_EL1`, `FAR_EL1`, `SPSR_EL1`.

### Phase 2 — Memory management baseline

Status: IN PROGRESS (started 2025-12-25)

Deliverables:

- Physical page allocator (PMM)
- MMU enabled with a simple mapping strategy (e.g. identity map for early kernel + higher-half optional)
- Safe separation of kernel vs user virtual ranges (even if userland comes later)

Acceptance:

- Kernel can allocate/free pages; basic paging works without crashes.

Notes:

- PMM implemented and initialized from DTB-reported RAM range (QEMU).
- MMU is now enabled with an identity map (TTBR0) plus a TTBR1 “higher-half” mapping of the same low physical region.
- UART remains usable because the peripheral window is mapped as device memory.
- I/D caches are enabled after cache maintenance; a small higher-half selftest reads a global via `KERNEL_VA_BASE + VA` and matches.
- Kernel/user VA split (separate user address space, ASIDs, EL0 mappings) is still pending for Phase 2 completion.

### Phase 3 — Enter EL0 + minimal syscalls (`exit`, `write`)

Status: DONE (2025-12-25)

Deliverables:

- Create an EL0 task with its own stack
- Syscall entry via EL0 `svc #0`
- Implement:
  - `exit` / `exit_group`
  - `write` to fd 1/2 (serial)

Acceptance:

- A tiny Linux-AArch64-style test binary prints to stdout and exits.

Notes:

- Implemented EL0 entry via `eret` and an EL1 exception fast-path for EL0 SVC.
- Implemented minimal Linux-style syscalls: `write` (fd 1/2 → UART) and `exit_group`.
- Validated with a tiny position-independent user-mode blob copied into a user-accessible sandbox region.

### Phase 4 — Initramfs filesystem (CPIO `newc`)

Status: DONE (2025-12-25)

Deliverables:

- Load/init CPIO archive from a known location (initially embedded; later from boot medium)
- VFS read-only: path resolution + file read + directory listing
- Syscalls (minimal set): `openat`, `close`, `read`, `newfstatat`, `getdents64`, `lseek`

Acceptance:

- `cat /hello.txt` works from initramfs.
- `ls /` lists initramfs entries.

Notes:

- Host-side builder added (`tools/mkcpio_newc.py`) producing `userland/build/initramfs.cpio` from `userland/initramfs/`.
- Kernel now embeds the CPIO archive and implements `openat`/`read`/`close` backed by initramfs.
- A syscall-only userland `cat` program is embedded as the EL0 payload and successfully prints `/hello.txt` in QEMU.

Follow-ups:

- Implemented `getdents64` + `newfstatat` + `lseek` (initramfs-backed) and a syscall-only `ls` that lists `/` via `getdents64`.
- Payload selection now supports `make run USERPROG=ls`.

### Phase 5 — ELF loader + `execve`

Status: IN PROGRESS (started 2025-12-25)

Deliverables:

- ELF64 loader for `ET_EXEC` first (initramfs-backed)
- `execve(221)` that replaces the current EL0 image
- User stack setup for `execve(argv, envp)`
- Minimal auxv (enough for static binaries)

Acceptance:

- `/init` in initramfs can `execve("/bin/ls", 0, 0)` and run it.
- `/init` in initramfs can `execve("/bin/echo", ["echo","hello"], 0)` once argv is implemented.

Notes:

- UART stdin is now wired to `read(fd=0)` (serial RX).
- Minimal `ET_EXEC` loader exists; `execve` loads binaries from initramfs into the user sandbox and updates `ELR_EL1`/`SP_EL0` for the next `eret`.
- Current `execve` is intentionally minimal: argv/envp stack layout + auxv are not built yet.

### Phase 6 — Processes: `fork` + `wait4`

Deliverables:

- Process table, PID allocation
- `fork`/`wait4` semantics sufficient for a simple shell
- Cooperative scheduler at syscall boundaries initially

Acceptance:

- `/bin/sh -c "/bin/echo hello"` works.

### Phase 7 — Pipes + fd duplication

Deliverables:

- `pipe2`, `dup2`
- Pipe buffers + blocking semantics (simple)

Acceptance:

- `/bin/sh -c "/bin/echo hello | /bin/cat"` works.

### Phase 8 — Compatibility hardening

Deliverables:

- Fill in common “small program” syscall gaps as they arise:
  - `mmap/munmap` + `brk`
  - `getpid/getppid`
  - `getcwd/chdir`
  - `uname`
  - `clock_gettime`/`nanosleep` (minimal)
  - basic `ioctl` subset for tty detection

Acceptance:

- A curated set of static Linux AArch64 binaries runs unmodified.

### Phase 9 — Move from QEMU to real Zero 2 W

Deliverables:

- Boot reliably on hardware from FAT boot partition
- Real UART mapping and clocking confirmed
- Interrupt controller + timer validated on real SoC
- Minimal SD/MMC read support **optional** (initramfs-only is acceptable initially)

Acceptance:

- Same kernel image boots on the Pi Zero 2 W and provides an interactive shell over UART.

## Key risks / unknowns (call these out early)

- **`raspi3b` vs real Zero 2 W differences**: QEMU’s model isn’t perfect; avoid leaning on devices not present on the Zero 2 W.
- **Exception level + firmware quirks**: real Pi boot state differs from QEMU; handle EL2/EL1 cleanly.
- **ELF/PIE**: supporting dynamically linked PIE binaries explodes scope (ld.so, relocations, TLS). Keep the initial goal static.
- **Syscall semantics drift**: Linux behavior can be subtle (errno, flags). Prefer “good enough for chosen binaries” over completeness.

## Suggested repo layout (future)

This repo currently contains tooling and a Linux kernel image used for QEMU bring-up. For the custom kernel, a clean separation helps:

- `kernel-aarch64/` (new): your miniature kernel sources
- `user-aarch64/` (optional): tiny Linux-AArch64 test binaries + initramfs builder
- keep `archive/reference-x86/kernel/` as a working reference

## Near-term next action

- Finish Phase 5 `execve` stack setup:
  - build an initial user stack with `argc/argv/envp`
  - add a minimal auxv surface (start small; expand only when required)
- Then move to Phase 6 (`fork` + `wait4`) to unlock a real shell.
