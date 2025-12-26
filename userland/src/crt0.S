.section .text.startup, "ax"

.global _start
.type _start, %function

/*
 * Minimal freestanding entrypoint.
 * Kernel enters EL0 with SP_EL0 already set.
 */
_start:
    /* Stack on entry (Linux-style):
     *   sp[0] = argc
     *   sp[1..] = argv pointers (argc entries) + NULL
     *   then envp pointers + NULL
     */
    ldr x0, [sp]          /* argc */
    add x1, sp, #8        /* argv */
    add x2, x1, x0, lsl #3
    add x2, x2, #8        /* envp = argv + (argc+1)*8 */

    /* Keep the original argument block addressable, but ensure
     * SP is 16-byte aligned before calling into C.
     */
    mov x19, sp
    and sp, x19, #-16

    bl  main

    /* exit_group((uint64_t)ret) */
    mov x8, #94
    svc #0

1:
    b 1b

.size _start, . - _start
