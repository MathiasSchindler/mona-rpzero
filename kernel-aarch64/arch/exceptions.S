.section .text.boot

/*
 * AArch64 exception vector table for EL1.
 * Must be 2KB-aligned.
 *
 * Layout (AArch64):
 *  - current EL using SP0   (EL1t): 4 entries
 *  - current EL using SPx   (EL1h): 4 entries
 *  - lower EL using AArch64 (EL0_64): 4 entries
 *  - lower EL using AArch32 (EL0_32): 4 entries
 */

.global vectors
.type vectors, %function

.align 11
vectors:
    /*
     * Each vector entry must be 0x80 bytes apart.
     * We place a single branch instruction and pad the rest.
     */

    /* EL1t */
    b   _exc_sync_el1t
    .space 0x80 - 4
    b   _exc_irq_el1t
    .space 0x80 - 4
    b   _exc_fiq_el1t
    .space 0x80 - 4
    b   _exc_serr_el1t
    .space 0x80 - 4

    /* EL1h */
    b   _exc_sync_el1h
    .space 0x80 - 4
    b   _exc_irq_el1h
    .space 0x80 - 4
    b   _exc_fiq_el1h
    .space 0x80 - 4
    b   _exc_serr_el1h
    .space 0x80 - 4

    /* EL0 (AArch64) */
    b   _exc_sync_el0_64
    .space 0x80 - 4
    b   _exc_irq_el0_64
    .space 0x80 - 4
    b   _exc_fiq_el0_64
    .space 0x80 - 4
    b   _exc_serr_el0_64
    .space 0x80 - 4

    /* EL0 (AArch32) */
    b   _exc_sync_el0_32
    .space 0x80 - 4
    b   _exc_irq_el0_32
    .space 0x80 - 4
    b   _exc_fiq_el0_32
    .space 0x80 - 4
    b   _exc_serr_el0_32
    .space 0x80 - 4

/* IDs for reporting */
.equ EXC_SYNC_EL1T,   0
.equ EXC_IRQ_EL1T,    1
.equ EXC_FIQ_EL1T,    2
.equ EXC_SERR_EL1T,   3
.equ EXC_SYNC_EL1H,   4
.equ EXC_IRQ_EL1H,    5
.equ EXC_FIQ_EL1H,    6
.equ EXC_SERR_EL1H,   7
.equ EXC_SYNC_EL0_64, 8
.equ EXC_IRQ_EL0_64,  9
.equ EXC_FIQ_EL0_64,  10
.equ EXC_SERR_EL0_64, 11
.equ EXC_SYNC_EL0_32, 12
.equ EXC_IRQ_EL0_32,  13
.equ EXC_FIQ_EL0_32,  14
.equ EXC_SERR_EL0_32, 15

/* Entry stubs: put kind in x16 to preserve x0.. syscall args. */
_exc_sync_el1t:   mov x16, #EXC_SYNC_EL1T;   b _exc_common
_exc_irq_el1t:    mov x16, #EXC_IRQ_EL1T;    b _exc_common
_exc_fiq_el1t:    mov x16, #EXC_FIQ_EL1T;    b _exc_common
_exc_serr_el1t:   mov x16, #EXC_SERR_EL1T;   b _exc_common

_exc_sync_el1h:   mov x16, #EXC_SYNC_EL1H;   b _exc_common
_exc_irq_el1h:    mov x16, #EXC_IRQ_EL1H;    b _exc_common
_exc_fiq_el1h:    mov x16, #EXC_FIQ_EL1H;    b _exc_common
_exc_serr_el1h:   mov x16, #EXC_SERR_EL1H;   b _exc_common

_exc_sync_el0_64: mov x16, #EXC_SYNC_EL0_64; b _exc_common
_exc_irq_el0_64:  mov x16, #EXC_IRQ_EL0_64;  b _exc_common
_exc_fiq_el0_64:  mov x16, #EXC_FIQ_EL0_64;  b _exc_common
_exc_serr_el0_64: mov x16, #EXC_SERR_EL0_64; b _exc_common

_exc_sync_el0_32: mov x16, #EXC_SYNC_EL0_32; b _exc_common
_exc_irq_el0_32:  mov x16, #EXC_IRQ_EL0_32;  b _exc_common
_exc_fiq_el0_32:  mov x16, #EXC_FIQ_EL0_32;  b _exc_common
_exc_serr_el0_32: mov x16, #EXC_SERR_EL0_32; b _exc_common

.extern exception_report
.extern exception_handle

/*
 * Exception entry convention:
 *   x16 = kind (set by per-vector stub)
 *   x0..x30 = interrupted context
 *
 * We save a full GPR frame for EL0 SVC so we can return.
 */
_exc_common:
    /* Reserve space for 31 GPRs + SP_EL0 (32 * 8 = 256). */
    sub sp, sp, #256

    stp x0,  x1,  [sp, #0]
    stp x2,  x3,  [sp, #16]
    stp x4,  x5,  [sp, #32]
    stp x6,  x7,  [sp, #48]
    stp x8,  x9,  [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]

    mrs x20, ESR_EL1
    mrs x21, ELR_EL1
    mrs x22, FAR_EL1
    mrs x23, SPSR_EL1
    mrs x19, SP_EL0
    str x19, [sp, #248]

    /* Fast path: SVC from EL0 AArch64 (kind=8, ESR.EC=0x15). */
    cmp x16, #EXC_SYNC_EL0_64
    b.ne 2f
    lsr x0, x20, #26
    cmp x0, #0x15
    b.ne 2f

    /* exception_handle(tf, kind, esr, elr, far, spsr) */
    mov x0, sp
    mov x1, x16
    mov x2, x20
    mov x3, x21
    mov x4, x22
    mov x5, x23
    bl  exception_handle

    /* x0==1 => return to interrupted context */
    cmp x0, #1
    b.ne 3f

    ldp x0,  x1,  [sp, #0]
    ldp x2,  x3,  [sp, #16]
    ldp x4,  x5,  [sp, #32]
    ldp x6,  x7,  [sp, #48]
    ldp x8,  x9,  [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    /* Restore SP_EL0 (may have been updated by syscall handlers). */
    ldr x19, [sp, #248]
    msr SP_EL0, x19
    /* Now restore remaining GPRs, including the real user x19. */
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    add sp, sp, #256
    eret

2:
    /* Report and halt. */
    mov x0, x16
    mov x1, x20
    mov x2, x21
    mov x3, x22
    mov x4, x23
    bl  exception_report

3:
    add sp, sp, #256
1:
    wfe
    b 1b

.size vectors, . - vectors
