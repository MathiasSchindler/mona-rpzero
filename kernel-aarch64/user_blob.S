.section .rodata.userblob, "a"

.global user_blob_start
.global user_blob_end

/*
 * Tiny Linux-style AArch64 user program:
 *   write(1, "hello from EL0\n", len)
 *   exit_group(0)
 *
 * Syscall ABI:
 *   x8 = nr, x0.. args, svc #0
 */

user_blob_start:
    mov     x0, #1
    adr     x1, 1f
    mov     x2, #(2f - 1f)
    mov     x8, #64          /* __NR_write */
    svc     #0

    mov     x0, #0
    mov     x8, #94          /* __NR_exit_group */
    svc     #0

0:
    b       0b

1:
    .ascii  "hello from EL0\n"
2:

user_blob_end:
