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

For a step-by-step plan to add framebuffer graphics and a FullHD terminal (QEMU-first, Pi later), see: [video.md](video.md)

## Quickstart (QEMU-first)

This repo is designed for a **kernel-first bare-metal workflow**: custom kernel + tiny syscall-only userland, loaded directly by QEMU (no Linux, no SD image required).

### Prerequisites

- `qemu-system-aarch64`
- AArch64 bare-metal cross toolchain (recommended: `aarch64-elf-*`)
- `python3` (used for initramfs tooling)

macOS (Homebrew) example:

```bash
brew install qemu aarch64-elf-gcc
```

### Build / Run

```bash
make
make run
```

Clean:

```bash
make clean
```

Intentional fault (exception dump smoke test):

```bash
make run TEST_FAULT=1
```

### Notes on QEMU (`-M raspi3b`)

- We use `-M raspi3b` because the Pi Zero 2 W is “Pi 3-ish” (Cortex-A53). It is the most practical, stable QEMU model for fast iteration.
- QEMU currently expects **1 GiB RAM** for `raspi3b` in this setup. This does not match real Zero 2 W RAM, but is fine for emulation.

### Make overrides

- `DTB=...` path to the device tree blob passed to QEMU via `-dtb`
- `MEM=1024` RAM in MiB for QEMU (default: 1024)

Examples:

```bash
make run MEM=1024
make run DTB=archive/bcm2710-rpi-zero-2-w.dtb
```

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

- Implemented in `kernel-aarch64/` and validated via `make run`.
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

Implemented (summary):

- EL normalization works (EL2→EL1 when applicable), with early `VBAR_EL1` installation.
- Exception dumps are readable and include `ESR_EL1`, `ELR_EL1`, `FAR_EL1`, `SPSR_EL1`.

### Phase 2 — Memory management baseline

Status: DONE (basic) (2025-12-25)

Deliverables:

- Physical page allocator (PMM)
- MMU enabled with a simple mapping strategy (e.g. identity map for early kernel + higher-half optional)
- Safe separation of kernel vs user virtual ranges (even if userland comes later)

Acceptance:

- Kernel can allocate/free pages; basic paging works without crashes.

Notes:

Implemented (summary):

- PMM initializes from DTB-reported RAM.
- MMU + caches are enabled with identity + higher-half mapping; UART stays mapped as device memory.
- User address space remains intentionally coarse; per-process TTBR0 switching exists but is not yet a full VM.

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

Implemented (summary):

- EL0 entry via `eret` and EL0 SVC handling is in place.
- Minimal Linux-style syscall ABI works (initially `write` + `exit_group`) for syscall-only user payloads.

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

Implemented (summary):

- Initramfs (embedded CPIO `newc`) is wired in; host builder exists (`tools/mkcpio_newc.py`).
- Read-only filesystem syscalls work (`openat/read/close/newfstatat/getdents64/lseek`) against initramfs.
- User payload selection supports `make run USERPROG=...`.

### Phase 5 — ELF loader + `execve`

Status: DONE (2025-12-26)

Deliverables:

- ELF64 loader for `ET_EXEC` first (initramfs-backed)
- `execve(221)` that replaces the current EL0 image
- User stack setup for `execve(argv, envp)`
- Minimal auxv (enough for static binaries)

Acceptance:

- `/init` in initramfs can `execve("/bin/ls", 0, 0)` and run it.
- `/init` in initramfs can `execve("/bin/echo", ["echo","hello"], 0)` once argv is implemented.

Notes:

Implemented (summary):

- ELF64 `ET_EXEC` loading from initramfs works, including stack setup for `argc/argv/envp` + minimal auxv.
- Serial RX is wired to `read(0)` so interactive userland is possible.

### Phase 6 — Processes: `fork` + `wait4`

Status: DONE (minimal 2-process model) (2025-12-26)

Deliverables:

- Process table, PID allocation
- `fork`/`wait4` semantics sufficient for a simple shell
- Cooperative scheduler at syscall boundaries initially

Acceptance:

- `/bin/sh -c "/bin/echo hello"` works.

Notes:

Implemented (summary):

- Process table + PID model exist.
- `fork` (restricted `clone(SIGCHLD, ...)`) and `wait4` are sufficient for shell-style execution.

### Phase 7 — Pipes + fd duplication

Status: DONE (2025-12-27)

Deliverables:

- `pipe2`, `dup2`
- Pipe buffers + blocking semantics (simple)

Acceptance:

- `/bin/sh -c "/bin/echo hello | /bin/cat"` works.

Notes:

Implemented (summary):

- `pipe2` + `dup2` enable simple pipelines.
- Current pipe semantics are minimal (flags=0 only; `-EAGAIN` retry model in userland).

### Phase 8 — Compatibility hardening

Status: IN PROGRESS (2025-12-27)

Deliverables:

- Fill in common “small program” syscall gaps as they arise:
  - `mmap/munmap` + `brk`
  - `getpid/getppid`
  - `getcwd/chdir`
  - `uname`
  - `clock_gettime`/`nanosleep` (monotonic + basic sleep)
  - basic `ioctl` subset for tty detection

Acceptance:

- A curated set of static Linux AArch64 binaries runs unmodified.

Notes:

 - Implemented: `getpid/getppid`, `uname`, `clock_gettime` (monotonic time since boot via the AArch64 generic timer; `CLOCK_REALTIME` is currently boot-relative until an RTC/NTP story exists), `brk`.
- Implemented: `getcwd`/`chdir` (per-process cwd + relative path resolution for `openat`/`newfstatat`/`execve`).
- Implemented (minimal): anonymous `mmap/munmap` (private+anonymous only, `addr=0` only, no file-backed mappings, no real page unmapping yet — address-space allocator).
 - Implemented: `nanosleep` (blocks the calling task until the deadline; cooperative scheduling; writes `{0,0}` to rem when provided).
- Implemented (minimal): `ioctl` tty subset for UART fds (`TCGETS`, `TIOCGWINSZ`, `TIOCGPGRP`).
- Implemented (minimal): `getuid/geteuid/getgid/getegid/gettid` (all IDs are 0; tid==pid).
- Implemented (minimal): `set_tid_address` (stores clear_child_tid; best-effort clears it on exit).
- Implemented (minimal): `set_robust_list`, `rt_sigaction`, `rt_sigprocmask` (stubs to keep simple static runtimes happy).
- Implemented (minimal): `getrandom` (xorshift-based bytes, not cryptographically secure).
- Syscall-only tool status and smoke-test binaries are tracked in `tools.md`.

#### Phase 8 follow-up idea: syscall-only “tool suite” (monacc-style)

You mentioned a large set of syscall-only tools you built for x86-64 in your related monacc project. That’s an excellent fit for this repo too, because it keeps the “no libc” stance intact and gives us concrete, testable syscall targets.

The practical way to tackle it here is to group tools by required kernel features and grow the kernel only as needed.

Suggested priority groups:

- **Group A: Already feasible on today’s kernel** (read-only initramfs + tty)
  - Examples: `cat`, `ls`, `echo`, `true/false`, `uname`, `pwd` (uses `getcwd`), `sleep` (uses `nanosleep`), simple `stat`/`readlink` variants that only read metadata.

- **Group B: Needs basic writable filesystem syscalls** (still small, high ROI)
  - Kernel work: `mkdirat`, `unlinkat`, `renameat2` (or `renameat`), `linkat`, `symlinkat`, `readlinkat`, `fchmodat`, `fchownat`, `utimensat`.
  - Tools unlocked: `mkdir`, `rmdir`, `rm`, `mv`, `cp`, `ln`, `touch`, `chmod`, `chown`, `readlink`.
  - Note: our current filesystem is initramfs read-only; we’ll likely want a tiny writable in-memory FS (tmpfs/ramfs) or an overlay-on-initramfs.

- **Group C: “procps-ish” tools** (requires a kernel story for process enumeration)
  - Kernel work options: (1) add a small custom syscall like `proc_list`, or (2) implement a minimal `/proc` view.
  - Tools: `ps`, `pstree`, `uptime`, `free`.

- **Group D: networking tools** (big scope)
  - Kernel work: `socket` family syscalls and a network device model; likely a separate phase.
  - Tools: `wget6`, `ping6`, `nc6`, `dns6`, `ntp6`, etc.

This lets us keep Phase 8 focused: first build out Groups A/B to get a useful “coreutils-ish” baseline, then decide whether Group C should be done via a syscall or `/proc`.

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

- Continue Phase 8 by porting/adding tools from `tools.md` and implementing only the kernel features they force:
  - next high-ROI step is a tiny writable FS (tmpfs/overlay) + a first batch of `*at` syscalls (`mkdirat/unlinkat/renameat*`).

## SMP-friendly evolution plan (avoid big rewrites)

Goal: eventually use **all 4 Cortex-A53 cores** (SMP), while keeping the codebase evolvable without “stop-the-world” refactors.

The core idea is: **design as if SMP exists**, but keep the runtime as UP (single core) until the foundations are ready.

### Design principles (with rationale)

1. **Make per-CPU state explicit early**
   - Rationale: most painful SMP rewrites come from global singletons like “current process”, “current stack”, “current interrupt nesting”.
   - Rule: anything that is “current” should live in a `cpu_t` structure, even if only CPU0 is used initially.

2. **Introduce locking APIs early (even if they are no-ops in UP)**
   - Rationale: adding locks later is a repo-wide invasive change.
   - Rule: shared mutable state must be accessed through functions that can take locks (`spinlock_t`, `irq_save/restore`), even if the lock compiles to nothing in UP mode.

3. **Keep scheduling/context switch encapsulated**
   - Rationale: SMP and preemption mostly change *how* you pick the next runnable task, not what a task is.
   - Rule: a small set of scheduler entry points (`sched_yield`, `sched_pick_next(cpu)`, `sched_switch_to(task)`) are the only places that touch runqueues and “current”.

4. **Make MMU changes go through a single mapping API**
   - Rationale: SMP requires TLB shootdowns (IPI) after page table updates.
   - Rule: do not scatter page-table writes; centralize them behind `vm_map/vm_unmap` so you can later add “broadcast TLB invalidate” in one place.

5. **Start with coarse-grained correctness; refine later**
   - Rationale: a correct coarse-grained lock is better than no lock. Later you can optimize to per-CPU/per-structure locks.

### Step-by-step implementation plan

The plan is split into “SMP-ready refactors” (no new hardware features) and “SMP bring-up” (secondary cores, IPIs).

#### Track A — SMP-ready refactors (keep running on 1 core)

**A1. Introduce `cpu_t` and `task_t` containers**
- What:
  - Add `cpu_t cpus[MAX_CPUS]` and migrate global singletons to `cpu_t`:
    - current task pointer/index
    - irq nesting depth
    - scheduler state / idle stack
  - Add `task_t` / `proc_t` as the home for per-task state:
    - trap frame, pid, state
    - address space handle (TTBR0)
    - file descriptor table pointer
- Why: reduces future SMP diff to “boot more CPUs + change scheduler policy”.

**A2. Make file descriptors per-task (or per-process) instead of global**
- What:
  - Replace the global `g_fds[]` with `fdtable_t` owned by init and inherited by fork.
  - Add reference counting if needed later (for `dup2`/pipes).
- Why: global FD state is a guaranteed rewrite once you add multiple processes and cores.

**A3. Add a minimal locking layer**
- What:
  - Implement `spinlock_t` and `spin_lock_irqsave/spin_unlock_irqrestore`.
  - In UP mode, the lock can be a debug-only assert + IRQ masking.
- Why: lets you add SMP incrementally without changing call sites.

**A4. Generalize process model beyond “init + one child”**
- What:
  - Grow from `g_procs[2]` to a small table (e.g. 32/64 tasks).
  - Keep scheduling cooperative at syscall boundaries initially.
- Why: pipes and a real shell pipeline require multiple runnable tasks.

**A5. Implement Phase 7 (`pipe2`, `dup2`) on top of A2/A4**
- What:
  - Add `pipe2`: allocate a pipe object with a ring buffer and two FD endpoints.
  - Add `dup2`: create a new FD entry pointing at the same underlying object.
  - Extend shell: `cmd1 | cmd2` spawns two children and wires stdout/stdin.
- Why: this forces the FD model to become “real”, which is a prerequisite for SMP-safe IO.

Acceptance for Track A:
- `/bin/sh -c "/bin/echo hello | /bin/cat"` works.
- Multiple children can run and be waited on (basic job control is optional).

#### Track B — SMP bring-up (enable additional cores)

**B1. Bring up secondary CPUs (CPU1..CPU3)**
- What:
  - Add boot path for secondary cores (on real hardware: mailbox/spin-table; on QEMU raspi3b this may be limited).
  - Each CPU gets its own stack and enters a common `cpu_secondary_main()`.
- Why: establishes the per-CPU execution environment.

**B2. Per-CPU timer and preemption hooks**
- What:
  - Initialize the AArch64 generic timer per CPU.
  - Optional: enable preemption by requesting reschedule at timer tick.
- Why: without a timer, CPU load balancing and fair scheduling are hard.

**B3. Inter-processor interrupts (IPIs)**
- What:
  - Implement minimal IPIs for:
    - reschedule (wake another CPU)
    - TLB shootdown (after page table updates)
- Why: required for correctness once multiple CPUs touch VM and runqueues.

**B4. Scheduler policy: global runqueue → per-CPU runqueues**
- What:
  - Start with a single global runqueue protected by a lock.
  - Later: migrate to per-CPU runqueues + work stealing.
- Why: minimizes early complexity; performance improvements can come later.

Acceptance for Track B:
- All cores reach `cpu_idle()` and can run user tasks.
- Concurrency smoke tests (e.g. multiple `echo` processes) complete without corruption.

### Practical notes / constraints

- QEMU `raspi3b` is great for early work, but not all Raspberry Pi hardware details (interrupt controller, secondary core bring-up) behave exactly like real hardware.
  Expect to validate true SMP bring-up on real Pi hardware.
- The best way to avoid future pain is to keep “UP today” but “SMP-shaped code” now.
