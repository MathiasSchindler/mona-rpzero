#include "irq.h"

#include "console_in.h"
#include "time.h"
#include "uart_pl011.h"

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

/* BCM2835 interrupt controller (in the 0x3Fxxxxxx peripheral window).
 * Used for peripheral IRQs like PL011 UART.
 */
#define IRQCTRL_BASE 0x3F00B200ull

#define IRQ_PENDING_2     (*(volatile uint32_t *)(uintptr_t)(IRQCTRL_BASE + 0x08ull))
#define ENABLE_IRQS_2     (*(volatile uint32_t *)(uintptr_t)(IRQCTRL_BASE + 0x14ull))

/* PL011 UART interrupt is IRQ 57 => pending2 bit 25. */
#define IRQ2_UART_BIT (1u << 25)

#ifndef TICK_HZ
#define TICK_HZ 100u
#endif

void irq_init(void) {
    /* Route the architectural timer interrupt to the core's IRQ line. */
    CORE0_TIMER_IRQ_CTRL |= (1u << 1);

    /* Start a periodic tick. */
    time_tick_init(TICK_HZ);

    /* Enable PL011 UART interrupts (RX) so blocked stdin can wake without polling. */
    ENABLE_IRQS_2 = IRQ2_UART_BIT;
    uart_irq_enable_rx();
}

void irq_handle(void) {
    uint32_t src = CORE0_IRQ_SOURCE;
    if (src & (1u << 1)) {
        /* AArch64 physical timer IRQ. Acknowledge and (re)arm as needed. */
        time_tick_handle_irq();
    }

    /* Peripheral IRQs (e.g. UART RX). */
    if (IRQ_PENDING_2 & IRQ2_UART_BIT) {
        (void)uart_irq_handle_rx(console_in_inject_char);
    }
}
