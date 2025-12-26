.section .text.boot
.global _start
.type _start, %function

.extern vectors

/*
 * QEMU (-M raspi3b) and Raspberry Pi firmware both typically jump into kernel8.img
 * in AArch64 state. We keep this extremely small for Phase 0.
 *
 * Convention (Linux-style):
 *   x0 = DTB pointer (often)
 */
_start:
    /* Preserve DTB pointer. */
    mov     x19, x0

    /* Set up a stack. */
    adrp    x1, __stack_top
    add     x1, x1, :lo12:__stack_top
    mov     sp, x1

    /* Clear .bss */
    adrp    x2, __bss_start
    add     x2, x2, :lo12:__bss_start
    adrp    x3, __bss_end
    add     x3, x3, :lo12:__bss_end
    mov     x4, xzr
1:
    cmp     x2, x3
    b.hs    2f
    str     x4, [x2], #8
    b       1b
2:

    /* Call C entry: kmain(dtb_ptr) */
    /* Mask interrupts for now. */
    msr     DAIFSet, #0xf

    /* Install exception vectors for EL1. */
    adrp    x0, vectors
    add     x0, x0, :lo12:vectors
    msr     VBAR_EL1, x0
    isb

    /* If we started at EL2, drop to EL1h. */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    and     x0, x0, #3
    cmp     x0, #2
    b.ne    2f

    /* Configure EL1 to be AArch64. */
    mrs     x0, HCR_EL2
    orr     x0, x0, #(1 << 31)   /* RW = 1 */
    msr     HCR_EL2, x0

    /* Set EL1 stack pointer. */
    msr     SP_EL1, x1

    /* Return to EL1h, with interrupts masked. */
    adr     x0, el1_entry
    msr     ELR_EL2, x0
    mov     x0, #0x3c5           /* DAIF=1111, M=0101 (EL1h) */
    msr     SPSR_EL2, x0
    isb
    eret

el1_entry:
    /* Now at EL1. Restore DTB pointer and run C. */
    mov     x0, x19
    bl      kmain

2:
    /* Already at EL1 (or unexpected EL); still try to run. */
    mov     x0, x19
    bl      kmain

    /* If kmain returns, park the CPU. */
3:
    wfe
    b 3b

.size _start, . - _start

.section .bss
.align 16
__stack:
    .skip 0x4000
__stack_top:
