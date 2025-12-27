#include "syscall.h"

static void write_u64_hex(uint64_t v) {
    char hx[16];
    const char *d = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        hx[15 - i] = d[(v >> (i * 4)) & 0xf];
    }
    (void)sys_write(1, hx, 16);
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    uint64_t cur = sys_brk(0);
    sys_puts("brk0=0x");
    write_u64_hex(cur);
    sys_puts("\n");

    uint64_t next = cur + 4096;
    uint64_t got = sys_brk((void *)(uintptr_t)next);
    sys_puts("brk1=0x");
    write_u64_hex(got);
    sys_puts("\n");

    return 0;
}
