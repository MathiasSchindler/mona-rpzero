#pragma once

#include "stdint.h"

/* Generic AArch64 timer based timekeeping.
 *
 * We use the architectural counter (CNT* registers) which is available in QEMU
 * and on Raspberry Pi (ARMv8).
 *
 * Semantics:
 * - time_now_ns(): monotonic nanoseconds since time_init() was called.
 */

void time_init(void);

uint64_t time_freq_hz(void);
uint64_t time_now_ns(void);

/* Periodic tick using the AArch64 physical timer (CNTP).
 *
 * The tick is used to wake the kernel out of `wfi` when there are sleeping
 * tasks or blocked stdin reads.
 */
void time_tick_init(uint32_t hz);
/* Ensure the timer is running in periodic mode (used when polling is needed). */
void time_tick_enable_periodic(void);

/* Program a one-shot tick delta from now (used for tickless sleep-only idle). */
void time_tick_schedule_oneshot_ns(uint64_t delta_ns);

/* Called from the timer IRQ handler to acknowledge and rearm/disable as needed. */
void time_tick_handle_irq(void);

/* Disable the timer interrupt source (used when waiting only for IRQ-driven IO). */
void time_tick_disable(void);
