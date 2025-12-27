.section .text

.global __syscall0
.type __syscall0, %function
__syscall0:
    /* x0 = nr */
    mov x8, x0
    mov x0, xzr
    svc #0
    ret

.global __syscall1
.type __syscall1, %function
__syscall1:
    /* x0 = nr, x1 = a0 */
    mov x8, x0
    mov x0, x1
    svc #0
    ret

.global __syscall2
.type __syscall2, %function
__syscall2:
    /* x0 = nr, x1 = a0, x2 = a1 */
    mov x8, x0
    mov x0, x1
    mov x1, x2
    svc #0
    ret

.global __syscall3
.type __syscall3, %function
__syscall3:
    /* x0 = nr, x1 = a0, x2 = a1, x3 = a2 */
    mov x8, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    svc #0
    ret

.global __syscall3_p
.type __syscall3_p, %function
__syscall3_p:
    /* Same as __syscall3, but meant to be declared with a typed pointer arg in C. */
    mov x8, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    svc #0
    ret

.global __syscall3_ppp
.type __syscall3_ppp, %function
__syscall3_ppp:
    /* (nr, p0, p1, p2) -> x8=nr, x0=p0, x1=p1, x2=p2 */
    mov x8, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    svc #0
    ret

.global __syscall4_upuu
.type __syscall4_upuu, %function
__syscall4_upuu:
    /* (nr, a0, p1, a2, a3) -> x0=a0, x1=p1, x2=a2, x3=a3 */
    mov x8, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    mov x3, x4
    svc #0
    ret

.global __syscall4_uppu
.type __syscall4_uppu, %function
__syscall4_uppu:
    /* (nr, a0, p1, p2, a3) -> x0=a0, x1=p1, x2=p2, x3=a3 */
    mov x8, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    mov x3, x4
    svc #0
    ret

.global __syscall4_upup
.type __syscall4_upup, %function
__syscall4_upup:
    /* (nr, a0, p1, a2, p3) -> x0=a0, x1=p1, x2=a2, x3=p3 */
    mov x8, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    mov x3, x4
    svc #0
    ret

.global __syscall4
.type __syscall4, %function
__syscall4:
    /* x0 = nr, x1 = a0, x2 = a1, x3 = a2, x4 = a3 */
    mov x8, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    mov x3, x4
    svc #0
    ret

.global __syscall5
.type __syscall5, %function
__syscall5:
    /* x0 = nr, x1 = a0, x2 = a1, x3 = a2, x4 = a3, x5 = a4 */
    mov x8, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    mov x3, x4
    mov x4, x5
    svc #0
    ret

.global __syscall6
.type __syscall6, %function
__syscall6:
    /* x0 = nr, x1 = a0, x2 = a1, x3 = a2, x4 = a3, x5 = a4, x6 = a5 */
    mov x8, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    mov x3, x4
    mov x4, x5
    mov x5, x6
    svc #0
    ret
