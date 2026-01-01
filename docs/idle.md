# Idle / Low-CPU “do nothing” (QEMU-first)

This repo is **QEMU-first** (raspi3b) and targets **AArch64** with the intent to run later on **Raspberry Pi Zero 2 W**.

The big practical issue: `make run` can peg a host CPU core even when the guest *looks* idle.

This document explains **why**, what “minimal kernel idle” really means in this codebase, and gives a step-by-step path to implement it.

## Why QEMU burns CPU when the guest is “idle”

QEMU (TCG) will execute guest instructions as fast as possible unless the guest reaches a state where it can **sleep**.

Two common reasons a small kernel pegs CPU:

- The kernel/userland is **busy-polling** (spinning) instead of blocking.
- The kernel never executes low-power/wait instructions like `wfi`/`wfe` in an “idle” path.

In this repo, both happen today.

## Current state in this repo (what causes the spinning)

These are the main hotspots:

- **Console input “blocking” is a spin-loop**
  - [kernel-aarch64/console_in.c](kernel-aarch64/console_in.c) implements `console_in_getc_blocking()` as `while (!try_getc) { /* spin */ }`.
  - `console_in_try_getc()` calls `console_in_poll()`, which actively polls UART and (optionally) USB keyboard.

- **Sleep uses busy-wait, not timer interrupts**
  - [kernel-aarch64/sched.c](kernel-aarch64/sched.c) explicitly busy-waits until the earliest sleep deadline (“keeps nanosleep functional without timer IRQs”).

- **IRQ exceptions aren’t handled as “returnable” events yet**
  - [kernel-aarch64/arch/exceptions.S](kernel-aarch64/arch/exceptions.S) only has a fast return path for EL0 SVC syscalls.
  - Any other exception kind (including IRQ) currently ends up in a report+halt path.
  - [kernel-aarch64/exceptions.c](kernel-aarch64/exceptions.c) similarly focuses on the EL0 SVC path.

- **Time is monotonic-only right now**
  - [kernel-aarch64/time.c](kernel-aarch64/time.c) reads the generic counter (`cntpct_el0`) but does not program a periodic timer interrupt.

Implication: adding a naive “idle task that does `wfi`” is not enough on its own:

- If userland is polling for input, it stays runnable forever → the scheduler never reaches “no runnable tasks”.
- If you make everything sleep without proper timer IRQ plumbing, `wfi` could sleep forever.

## Options (from easiest to most correct)

### Option A: QEMU-side throttling (fastest win)

If your goal is simply “don’t burn my laptop core while I’m at the prompt”, QEMU can be told to sleep/yield when the guest is spinning.

Typical approach:

- Run QEMU with instruction counting + sleeping enabled, e.g. `-icount shift=auto,sleep=on`.

Pros:
- No kernel changes.
- Big CPU reduction for spin-heavy guests.

Cons:
- It can change timing characteristics (usually fine for this project).
- It’s QEMU-only; real hardware still needs proper idle.

### Option B: “Minimal idle” (small code, limited effect)

A minimal idle loop is basically:

- If no runnable processes: enable interrupts and execute `wfi` (wait-for-interrupt).

Pros:
- Easy and architecturally correct.

Cons:
- In this repo today, it won’t reduce CPU much until you also stop busy-polling.

### Option C: Real low-CPU idle (recommended direction)

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

## Quick checklist: what to implement if you want the CPU drop first

If your priority is “host CPU drops while at prompt in QEMU”, the shortest practical path is:

1) Add QEMU `-icount ... sleep=on` support (Option A), or
2) Implement Step 1 + Step 5 (stop polling in `read()`), then Step 4.

Both are valid; (2) is more “real kernel” work and pays off for Pi Zero 2 W.
