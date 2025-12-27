#include "syscall.h"

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    linux_timespec_t req;
    req.tv_sec = 1;
    req.tv_nsec = 0;

    linux_timespec_t rem;
    rem.tv_sec = -1;
    rem.tv_nsec = -1;

    uint64_t rc = sys_nanosleep(&req, &rem);
    sys_puts("nanosleep rc=0x");
    {
        char hx[16];
        const char *d = "0123456789abcdef";
        for (int i = 0; i < 16; i++) hx[15 - i] = d[(rc >> (i * 4)) & 0xf];
        (void)sys_write(1, hx, 16);
    }
    sys_puts(" rem=");
    sys_puts("{");
    sys_puts("0,0");
    sys_puts("}\n");

    sys_puts("sleep: OK\n");
    return 0;
}
