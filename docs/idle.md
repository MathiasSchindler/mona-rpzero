# Idle / Low-CPU “do nothing” (QEMU-first)

This repo is **QEMU-first** (raspi3b) and targets **AArch64** with the intent to run later on **Raspberry Pi Zero 2 W**.

The big practical issue: `make run` can peg a host CPU core even when the guest *looks* idle.

This document explains **why**, what “minimal kernel idle” really means in this codebase, and gives a step-by-step path to implement it.

## Why QEMU burns CPU when the guest is “idle”

QEMU (TCG) will execute guest instructions as fast as possible unless the guest reaches a state where it can **sleep**.

Two common reasons a small kernel pegs CPU:

- The kernel/userland is **busy-polling** (spinning) instead of blocking.
- The kernel never executes low-power/wait instructions like `wfi`/`wfe` in an “idle” path.

Historically, this repo did both. The current implementation (Option C) fixes that by making sleeps and stdin reads *truly blocking* and by using `wfi` when nothing is runnable.

## What used to cause spinning (and what changed)

These are the main hotspots:

- **Console input “blocking” was a spin-loop**
  - [kernel-aarch64/console_in.c](kernel-aarch64/console_in.c) still contains a spin-based `console_in_getc_blocking()` helper, but the *syscall layer* now provides true blocking semantics for stdin reads.
  - The input backends are still polled (UART and optional USB keyboard), but polling is no longer done in a tight loop from userland.

- **Sleep used to be a busy-wait**
  - The scheduler used to busy-wait until the next sleep deadline.
  - This has been replaced with `wfi`-based idle plus a timer tick that wakes the kernel so it can reevaluate sleepers (periodic when polling is needed, one-shot when only sleepers exist).

- **IRQ exceptions were not returnable**
  - IRQs are now handled as returnable events while in EL1h (kernel context), which is what `wfi` idle relies on.
  - EL0 runs with IRQs masked (cooperative kernel), so we do not currently take timer IRQs while executing user code.

- **Time was monotonic-only**
  - The kernel now also programs the AArch64 physical timer (CNTP) for wakeups (periodic or one-shot depending on idle conditions).

Implication: adding a naive “idle task that does `wfi`” is not enough on its own:

- If userland is polling for input, it stays runnable forever → the scheduler never reaches “no runnable tasks”.
- If you make everything sleep without proper timer IRQ plumbing, `wfi` could sleep forever.

## Option C: Real low-CPU idle (current direction)

This is the full solution:

- Add a real timer interrupt path.
- Make blocking operations actually **block** (sleep the task) instead of busy polling.
- When nothing runnable: `wfi`.

This moves the project toward “Pi-later” correctness while also fixing QEMU CPU usage.

## Step-by-step implementation guide (Option C)

This is written to be incremental: each step should keep the kernel bootable in QEMU.

### Step 0 — Keep the project’s style constraints in mind

From [docs/plan.md](plan.md) and [docs/work.md](work.md), the guiding constraints are:

- No external runtime deps (no libc, no third-party libraries).
- Freestanding C + small AArch64 asm.
- QEMU-first for iteration; Pi Zero 2 W soon.
- Prefer simple, deterministic semantics.

Practical style notes:

- Prefer tiny helpers over “frameworks”.
- Avoid pulling in libgcc-heavy helpers (see the comment in [kernel-aarch64/time.c](kernel-aarch64/time.c) about avoiding 128-bit division helpers).
- Add acceptance checks via `make test` / `kinit` when feasible.

### Step 1 — Make IRQ exceptions returnable (plumbing)

Goal: an IRQ should be able to interrupt EL0, run a handler, and return to EL0.

What to change:

- Extend the assembly path in [kernel-aarch64/arch/exceptions.S](kernel-aarch64/arch/exceptions.S):
  - Today, only “EL0 SVC” calls into `exception_handle()` and returns.
  - Add a second fast path for `kind == EXC_IRQ_EL0_64` (and optionally EL1 IRQ kinds).
  - For that IRQ path:
    - call into a C handler (e.g. `irq_handle(tf, kind, esr, elr, far, spsr)` or reuse `exception_handle()`),
    - restore registers,
    - `eret` back to the interrupted context.

- Update [kernel-aarch64/exceptions.c](kernel-aarch64/exceptions.c) to handle `kind == 9` (IRQ_EL0_64):
  - Do *not* treat it as fatal.
  - Call a timer/input IRQ dispatcher.

Acceptance:
- With IRQs still disabled at the source, the kernel should behave exactly as before.
- With a synthetic IRQ enabled later, the kernel should not halt.

### Step status (what’s implemented today)

This is the current status of the Option C steps in this repo:

- **Step 1 (IRQ exception plumbing): implemented for EL1h IRQs**
  - `wfi` idle relies on taking IRQs in EL1h and returning.
  - Implementation:
    - EL1h IRQ entry is IRQ-safe and returnable in [kernel-aarch64/arch/exceptions.S](kernel-aarch64/arch/exceptions.S).
    - The C dispatcher treats EL1h IRQ (kind 5) as non-fatal and calls `irq_handle()` in [kernel-aarch64/exceptions.c](kernel-aarch64/exceptions.c).
  - Important constraint:
    - EL0 runs with IRQs masked (cooperative kernel). Syscall return restores `SPSR_EL1` to `0x3c0` (EL0t with DAIF masked) in [kernel-aarch64/arch/exceptions.S](kernel-aarch64/arch/exceptions.S).
    - As a result, timer IRQs won’t preempt user code; they primarily wake the kernel out of `wfi`.

- **Step 2 (idle primitive): implemented**
  - `irq_enable()`, `irq_disable()`, and `cpu_wfi()` exist in [kernel-aarch64/include/irq.h](kernel-aarch64/include/irq.h).
  - The scheduler uses these around the idle loop in [kernel-aarch64/sched.c](kernel-aarch64/sched.c).

- **Step 3 (periodic tick + IRQ source): implemented (CNTP + local-peripheral routing)**
  - The AArch64 physical timer is programmed via `time_tick_init()` plus either `time_tick_enable_periodic()` or `time_tick_schedule_oneshot_ns()` in [kernel-aarch64/time.c](kernel-aarch64/time.c).
  - The CNTPNSIRQ route for core 0 is enabled using the BCM2836/BCM2710 local peripheral registers (base `0x4000_0000`) in [kernel-aarch64/irq.c](kernel-aarch64/irq.c).
  - The periodic tick is initialized in `irq_init()` from [kernel-aarch64/main.c](kernel-aarch64/main.c).
  - MMU note: the local-peripheral MMIO window is mapped so those registers are accessible (see [kernel-aarch64/mmu.c](kernel-aarch64/mmu.c)).

- **Step 4 (remove scheduler busy-wait): implemented (`wfi` idle)**
  - When there are sleepers or blocked I/O but nothing runnable, the scheduler enables IRQs and executes `wfi` in [kernel-aarch64/sched.c](kernel-aarch64/sched.c).
  - This is what stops QEMU from pegging a host core when the guest is idle.

- **Step 5 (stdin truly blocking): implemented (blocked process state + wake/complete)**
  - `read()` on the UART-backed fd uses true blocking:
    - If no input is available, `sys_read()` marks the process `PROC_BLOCKED_IO`, records a pending read, and reschedules in [kernel-aarch64/sys_fs.c](kernel-aarch64/sys_fs.c).
    - The scheduler wakes one blocked reader when buffered input exists (still polled) and completes the pending read on context switch in [kernel-aarch64/sched.c](kernel-aarch64/sched.c).
  - Input buffering helpers are provided by `console_in_has_data()` / `console_in_pop()` in [kernel-aarch64/console_in.c](kernel-aarch64/console_in.c).

- **Step 6 (tickless idle): implemented (sleepers-only)**
  - When the system has sleepers but no runnable tasks, the scheduler programs a **one-shot** timer for the earliest sleep deadline, then executes `wfi`.
  - When the system is only blocked on stdin and the active input backend is **IRQ-driven** (PL011 UART RX), the kernel can disable the tick entirely and sleep in `wfi` until a UART IRQ arrives.
  - If any configured input backend still requires polling (currently: USB keyboard), the kernel keeps using a **periodic** tick while stdin is blocked so polling can continue.
  - The policy is implemented in [kernel-aarch64/sched.c](kernel-aarch64/sched.c) and uses the timer helpers in [kernel-aarch64/time.c](kernel-aarch64/time.c).

### Step 2 — Introduce an “idle” primitive (safe building block)

Add a small arch helper (suggested location: [kernel-aarch64/include/arch.h](kernel-aarch64/include/arch.h)):

- `arch_idle()` that executes `wfi` (or `wfe`) and a `arch_relax()` for spin loops.

Rules:
- Only call `wfi` when you are sure you have a wakeup source (timer IRQ, IO IRQ, etc.).
- Keep the helper `static inline` and freestanding.

### Step 3 — Add a periodic tick (generic timer) and an IRQ source

Goal: replace time-based busy waits with timer-driven wakeups.

Conceptually:

- Program the AArch64 generic timer to fire at a fixed frequency (e.g. 100 Hz or 1000 Hz).
- Ensure the interrupt controller routes that timer interrupt to the CPU.
- In the timer IRQ handler:
  - acknowledge/clear the timer interrupt,
  - wake sleepers (`sched_wake_sleepers()`),
  - optionally poll input backends (UART/USB) and wake blocked readers.

Where to start:

- You already have monotonic time in [kernel-aarch64/time.c](kernel-aarch64/time.c).
- You will likely add:
  - a minimal interrupt controller driver (for raspi3b QEMU this is typically a GIC variant),
  - a `time_tick_init(hz)` and `time_tick_ack()`.

Acceptance:
- Timer IRQs arrive without crashing.
- `nanosleep` no longer requires a busy-wait loop.

### Step 4 — Remove the scheduler busy-wait and replace it with idle

Update [kernel-aarch64/sched.c](kernel-aarch64/sched.c):

- Today, when only sleepers exist, the scheduler busy-waits until the earliest deadline.
- Change this behavior to:
  - if there are sleepers but no runnable tasks:
    - enter idle (`wfi`) and let the timer IRQ wake sleepers.

This is the key CPU reduction step for “sleep-heavy” workloads.

Acceptance:
- `sleep` and any `nanosleep` users still work.
- Host CPU usage drops when only sleeping tasks exist.

### Step 5 — Make console input truly blocking (stop busy polling)

Today, console input “blocking” is implemented as spinning + polling.

Goal: when a process does `read(0, ...)` and no input is available:

- put the process into a blocked state (new proc state, e.g. `PROC_BLOCKED_IO`),
- reschedule,
- wake it when input arrives.

Where the behavior comes from today:

- Input polling is in [kernel-aarch64/console_in.c](kernel-aarch64/console_in.c).

A practical incremental approach (still no external libs):

1) Add a per-process “blocked on console input” flag/state.
2) In `sys_read()` for fd 0, if no input is available:
   - mark current process blocked,
   - call the scheduler to switch.
3) On each timer tick IRQ:
   - call `console_in_poll()` to pull UART/USB input into the ring,
   - if the ring is non-empty, wake one (or all) processes blocked on console input.

This keeps USB keyboard support viable even if it remains polled (see the note in `usb_kbd.c` about being polled).

Acceptance:
- Sitting at a shell prompt should no longer peg the host CPU.
- Input remains responsive.

### Step 6 — (Optional) Tickless idle

Once the above works, you can reduce overhead further:

- If there are no runnable tasks and the next wakeup is a known sleep deadline, program the timer for exactly that deadline instead of a periodic tick.

This is optional and can wait until after the Pi bring-up.

## What’s “minimal” in practice?

- Minimal code change: an idle loop + `wfi`.
- Minimal change that actually helps: remove busy loops (sleep + console) so the kernel can reach idle.
- Minimal change that is *correct* on real hardware: real IRQ handling + real wakeup sources.

## Quick checklist (what matters for the CPU drop)

If your priority is “host CPU drops while at prompt in QEMU”, the key ingredients are:

- Stop spinning in “blocking” code paths (stdin reads and sleeps).
- Make sure the scheduler reaches an idle state and uses `wfi` with IRQs enabled.
- Provide a wake source (periodic tick is the current one).
