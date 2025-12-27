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

    const uint64_t PROT_READ = 0x1u;
    const uint64_t PROT_WRITE = 0x2u;
    const uint64_t MAP_PRIVATE = 0x02u;
    const uint64_t MAP_ANONYMOUS = 0x20u;

    uint64_t rc = sys_mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((int64_t)rc < 0) {
        sys_puts("mmap: failed rc=");
        write_u64_hex(rc);
        sys_puts("\n");
        return 1;
    }

    sys_puts("mmap: addr=0x");
    write_u64_hex(rc);
    sys_puts("\n");

    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)rc;
    p[0] = 0x12;
    p[1] = 0x34;
    p[2] = 0x56;
    p[3] = 0x78;

    if (p[0] != 0x12 || p[1] != 0x34 || p[2] != 0x56 || p[3] != 0x78) {
        sys_puts("mmap: readback mismatch\n");
        return 2;
    }

    uint64_t urc = sys_munmap((void *)(uintptr_t)rc, 4096);
    if ((int64_t)urc < 0) {
        sys_puts("munmap: failed rc=");
        write_u64_hex(urc);
        sys_puts("\n");
        return 3;
    }

    sys_puts("mmap: OK\n");
    return 0;
}
