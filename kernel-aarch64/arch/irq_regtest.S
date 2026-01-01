.section .text

.global irq_regtest
.type irq_regtest, %function

/*
 * int irq_regtest(void)
 *
 * Sanity-check that the EL1h IRQ entry/exit path preserves callee-saved
 * registers across a timer wakeup (wfi).
 *
 * Returns:
 *   0 on success, non-zero on failure.
 */
irq_regtest:
    /* Preserve callee-saved registers we clobber in this selftest. */
    stp     x29, x30, [sp, #-48]!
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]

    /* Set recognizable 64-bit patterns in a few callee-saved registers. */
    mov     x19, #0x1111
    movk    x19, #0x2222, lsl #16
    movk    x19, #0x3333, lsl #32
    movk    x19, #0x4444, lsl #48

    mov     x20, #0xaaaa
    movk    x20, #0xbbbb, lsl #16
    movk    x20, #0xcccc, lsl #32
    movk    x20, #0xdddd, lsl #48

    mov     x21, #0x0f0f
    movk    x21, #0x1e1e, lsl #16
    movk    x21, #0x2d2d, lsl #32
    movk    x21, #0x3c3c, lsl #48

    mov     x22, #0x1357
    movk    x22, #0x2468, lsl #16
    movk    x22, #0xdead, lsl #32
    movk    x22, #0xbeef, lsl #48

    mov     x23, #0xaaaa
    movk    x23, #0x5555, lsl #16
    movk    x23, #0x0ff0, lsl #32
    movk    x23, #0xf00f, lsl #48

    mov     x29, #0x1111
    movk    x29, #0xaaaa, lsl #16
    movk    x29, #0x2222, lsl #32
    movk    x29, #0xbbbb, lsl #48

    /* Enable IRQs and wait for the next tick. */
    msr     daifclr, #2
    wfi
    msr     daifset, #2

    /* Rebuild expected values in caller-saved regs for comparison. */
    mov     x0, #0x1111
    movk    x0, #0x2222, lsl #16
    movk    x0, #0x3333, lsl #32
    movk    x0, #0x4444, lsl #48

    mov     x1, #0xaaaa
    movk    x1, #0xbbbb, lsl #16
    movk    x1, #0xcccc, lsl #32
    movk    x1, #0xdddd, lsl #48

    mov     x4, #0x0f0f
    movk    x4, #0x1e1e, lsl #16
    movk    x4, #0x2d2d, lsl #32
    movk    x4, #0x3c3c, lsl #48

    mov     x5, #0x1357
    movk    x5, #0x2468, lsl #16
    movk    x5, #0xdead, lsl #32
    movk    x5, #0xbeef, lsl #48

    mov     x6, #0xaaaa
    movk    x6, #0x5555, lsl #16
    movk    x6, #0x0ff0, lsl #32
    movk    x6, #0xf00f, lsl #48

    mov     x7, #0x1111
    movk    x7, #0xaaaa, lsl #16
    movk    x7, #0x2222, lsl #32
    movk    x7, #0xbbbb, lsl #48

    cmp     x19, x0
    cset    w2, ne
    cmp     x20, x1
    cset    w3, ne
    orr     w0, w2, w3
    cmp     x21, x4
    cset    w2, ne
    orr     w0, w0, w2
    cmp     x22, x5
    cset    w2, ne
    orr     w0, w0, w2
    cmp     x23, x6
    cset    w2, ne
    orr     w0, w0, w2
    cmp     x29, x7
    cset    w2, ne
    orr     w0, w0, w2

    /* Restore caller's callee-saved registers. */
    ldp     x19, x20, [sp, #16]
    ldp     x21, x22, [sp, #32]
    ldp     x29, x30, [sp], #48
    ret

.size irq_regtest, . - irq_regtest
