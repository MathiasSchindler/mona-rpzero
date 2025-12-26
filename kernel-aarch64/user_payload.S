.section .rodata.userpayload, "a"

.global user_payload_start
.global user_payload_end

/*
 * Embedded EL0 payload.
 *
 * Built from top-level userland/ (syscall-only, linked at USER_REGION_BASE).
 */

user_payload_start:
    .incbin "build/user_payload.bin"
user_payload_end:
