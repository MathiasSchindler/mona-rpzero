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

Status: DONE (basic) (2025-12-25)

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
- Kernel/user VA split is still intentionally minimal (single coarse user region mapping).
  Per-process TTBR0 switching exists for `fork`/`wait4`, but this is not yet a full multi-process VM design.

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

- UART stdin is now wired to `read(fd=0)` (serial RX).
- Minimal `ET_EXEC` loader exists; `execve` loads binaries from initramfs into the user sandbox and updates `ELR_EL1`/`SP_EL0` for the next `eret`.
- `execve` now builds a Linux-style initial stack: `argc`, `argv[]`, `envp[]`, and a minimal `auxv`.
  Implemented `auxv` entries include `AT_PAGESZ`, `AT_ENTRY`, `AT_EXECFN`, `AT_PLATFORM`, `AT_RANDOM`, uid/gid placeholders and (best-effort) `AT_PHDR`/`AT_PHENT`/`AT_PHNUM`.

### Phase 6 — Processes: `fork` + `wait4`

Status: DONE (minimal 2-process model) (2025-12-26)

Deliverables:

- Process table, PID allocation
- `fork`/`wait4` semantics sufficient for a simple shell
- Cooperative scheduler at syscall boundaries initially

Acceptance:

- `/bin/sh -c "/bin/echo hello"` works.

Notes:

- Implemented `fork` via a restricted `clone(SIGCHLD, ...)` and `wait4` for a single child process.
- Userland now supports `sh -c "..."` for basic scriptability.

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

- Move to Phase 7 (pipes + fd duplication):
  - implement `pipe2` and `dup2`
  - teach the tiny shell to handle a single pipeline (`cmd1 | cmd2`) using those syscalls
  - acceptance: `/bin/sh -c "/bin/echo hello | /bin/cat"` works

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
