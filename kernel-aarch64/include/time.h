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
