#include "irq.h"

#include "time.h"

/*
 * BCM2836/BCM2710 local peripherals base.
 *
 * This is present on Raspberry Pi 3-class SoCs and the Raspberry Pi Zero 2 W.
 * QEMU (-M raspi3b) models it too.
 */
#define LOCAL_PERIPH_BASE 0x40000000ull

/* Core 0 timer interrupt control:
 * bit1 routes CNTPNSIRQ (non-secure physical timer) to IRQ.
 */
#define CORE0_TIMER_IRQ_CTRL (*(volatile uint32_t *)(uintptr_t)(LOCAL_PERIPH_BASE + 0x40ull))

/* Core 0 IRQ source:
 * bit1 indicates CNTPNSIRQ pending.
 */
#define CORE0_IRQ_SOURCE (*(volatile uint32_t *)(uintptr_t)(LOCAL_PERIPH_BASE + 0x60ull))

#ifndef TICK_HZ
#define TICK_HZ 100u
#endif

void irq_init(void) {
    /* Route the architectural timer interrupt to the core's IRQ line. */
    CORE0_TIMER_IRQ_CTRL |= (1u << 1);

    /* Start a periodic tick. */
    time_tick_init(TICK_HZ);
}

void irq_handle(void) {
    uint32_t src = CORE0_IRQ_SOURCE;
    if (src & (1u << 1)) {
        /* AArch64 physical timer IRQ. Rearm it to clear the interrupt. */
        time_tick_rearm();
    }
}
