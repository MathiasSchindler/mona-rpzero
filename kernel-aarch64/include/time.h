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
void time_tick_rearm(void);
