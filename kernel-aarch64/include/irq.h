#pragma once

#include "stdint.h"

/*
 * Minimal IRQ helpers.
 *
 * Current scope (Option C bring-up):
 * - Enable IRQs only around `wfi` in the scheduler idle loop.
 * - Handle the AArch64 physical timer interrupt via BCM2836/BCM2710 local
 *   interrupt routing (QEMU raspi3b + Pi Zero 2 W compatible).
 */

static inline void irq_enable(void) {
    /* Clear PSTATE.I (IRQ mask) and PSTATE.F (FIQ mask).
     * Some platforms route the physical timer interrupt as FIQ.
     */
    __asm__ volatile("msr daifclr, #3" ::: "memory");
}

static inline void irq_disable(void) {
    /* Set PSTATE.I (IRQ mask) and PSTATE.F (FIQ mask). */
    __asm__ volatile("msr daifset, #3" ::: "memory");
}

static inline void cpu_wfi(void) {
    __asm__ volatile("wfi" ::: "memory");
}

void irq_init(void);
void irq_handle(void);

int irq_regtest(void);
