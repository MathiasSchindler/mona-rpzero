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
  - This has been replaced with `wfi`-based idle plus a timer wakeup so it can reevaluate sleepers and/or poll-only input backends (one-shot to the next deadline; periodic is only a fallback when deadlines can’t be computed).

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

## How idle works today

This section describes how the kernel currently reduces CPU usage while staying responsive.

### Execution model (important constraint)

- EL0 runs with IRQs masked (cooperative kernel), so timer IRQs do not preempt user code.
- IRQs are primarily used to wake EL1 out of `wfi` when there is nothing runnable.

### Wake sources

- AArch64 physical timer (CNTP): used for sleep deadlines and for scheduling the next “poll-only” input check.
- PL011 UART RX IRQ: used to wake the kernel for UART input (so blocked stdin can be truly tickless when only UART input is relevant).
- Optional USB keyboard: currently polled (no IRQ wake). To keep overhead low, polling is rate-limited to a cadence and driven by one-shot timer wakeups.

### Scheduler idle policy

The scheduler loop in [kernel-aarch64/sched.c](kernel-aarch64/sched.c) does this each pass:

- Poll input once per pass via [kernel-aarch64/console_in.c](kernel-aarch64/console_in.c) (UART always, USB keyboard only when “due”).
- Wake sleepers whose deadline has passed.
- Wake at most one blocked stdin reader if buffered input exists.
- If a runnable process exists, run it.
- If nothing is runnable:
  - If there are no sleepers and no blocked I/O: return “no work”.
  - Otherwise enter idle: enable IRQs and execute `wfi`.

Before executing `wfi`, the scheduler selects how it will wake up next:

- If there are sleepers, schedule a one-shot timer to the earliest sleep deadline.
- If stdin is blocked:
  - If no polling input backend is enabled (UART-only): disable the timer tick entirely and rely on UART RX IRQ to wake.
  - If a polling backend is enabled (USB keyboard): schedule a one-shot timer to the next poll deadline.
- If both sleepers and USB polling are present, wake at the earliest of “sleep deadline” and “poll deadline”.

### Input behavior and latency

- UART input is IRQ-driven and can wake the kernel immediately.
- USB keyboard input is polled on a fixed cadence (currently 10ms), so the worst-case input latency is roughly one poll interval.

## What’s “minimal” in practice?

- Minimal code change: an idle loop + `wfi`.
- Minimal change that actually helps: remove busy loops (sleep + console) so the kernel can reach idle.
- Minimal change that is *correct* on real hardware: real IRQ handling + real wakeup sources.

## Quick checklist (what matters for the CPU drop)

If your priority is “host CPU drops while at prompt in QEMU”, the key ingredients are:

- Stop spinning in “blocking” code paths (stdin reads and sleeps).
- Make sure the scheduler reaches an idle state and uses `wfi` with IRQs enabled.
- Provide a wake source (UART RX IRQ and/or CNTP one-shot wakeups; periodic tick is only a fallback).

## Recommended runs to test idle behavior

Use one of these depending on what you want to validate.

**1) Lowest-noise “is idle really idle?” run (headless, no polled input):**

`make run AARCH64_CROSS=aarch64-linux-gnu- GFX=0 USB_KBD=0 USB_NET=0 SERIAL=stdio MONITOR=none`

Expected: when the shell is sitting at a prompt (stdin blocked), host CPU should drop very low because the kernel can disable the tick and sleep in `wfi` until UART RX IRQ.

**2) USB keyboard enabled (polling backend) run:**

`make run AARCH64_CROSS=aarch64-linux-gnu- GFX=1 USB_KBD=1 USB_NET=0 SERIAL=stdio MONITOR=none`

Expected: host CPU stays low, with periodic wakeups only at the USB poll cadence (default ~10ms) and/or earlier sleep deadlines.
