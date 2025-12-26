.section .text

.global enter_el0
.type enter_el0, %function

/*
 * void enter_el0(uint64_t entry, uint64_t user_sp);
 *
 * x0 = entry VA
 * x1 = user stack VA
 */
enter_el0:
    msr     sp_el0, x1
    msr     elr_el1, x0

    /* SPSR_EL1: return to EL0t, mask DAIF (debug/SError/IRQ/FIQ). */
    mov     x2, #0x3c0
    msr     spsr_el1, x2
    isb
    eret

.size enter_el0, . - enter_el0
