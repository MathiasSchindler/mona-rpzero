#include "syscall.h"

static void write_u64_dec(uint64_t v) {
    char buf[32];
    uint64_t n = 0;
    if (v == 0) {
        buf[n++] = '0';
    } else {
        char tmp[32];
        uint64_t t = v;
        uint64_t m = 0;
        while (t > 0 && m < sizeof(tmp)) {
            tmp[m++] = (char)('0' + (t % 10));
            t /= 10;
        }
        while (m > 0) {
            buf[n++] = tmp[--m];
        }
    }
    (void)sys_write(1, buf, n);
}

static void write_u64_hex(uint64_t v) {
    char hx[16];
    const char *d = "0123456789abcdef";
    for (int i = 0; i < 16; i++) hx[15 - i] = d[(v >> (i * 4)) & 0xf];
    (void)sys_write(1, hx, 16);
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    sys_puts("uid=");
    write_u64_dec(sys_getuid());
    sys_puts(" euid=");
    write_u64_dec(sys_geteuid());
    sys_puts(" gid=");
    write_u64_dec(sys_getgid());
    sys_puts(" egid=");
    write_u64_dec(sys_getegid());
    sys_puts("\n");

    sys_puts("tid=");
    write_u64_dec(sys_gettid());
    sys_puts("\n");

    uint32_t tidword = 0;
    uint64_t rc = sys_set_tid_address(&tidword);
    sys_puts("set_tid_address rc=");
    write_u64_dec(rc);
    sys_puts("\n");

    /* Minimal signal ABI probes (common in static runtimes). */
    uint8_t oldact[64];
    for (uint64_t i = 0; i < sizeof(oldact); i++) oldact[i] = 0xAA;
    rc = sys_rt_sigaction(2 /* SIGINT */, 0, oldact, 8);
    sys_puts("rt_sigaction rc=0x");
    write_u64_hex(rc);
    sys_puts("\n");

    uint8_t oldset[16];
    for (uint64_t i = 0; i < sizeof(oldset); i++) oldset[i] = 0xAA;
    rc = sys_rt_sigprocmask(0, 0, oldset, 8);
    sys_puts("rt_sigprocmask rc=0x");
    write_u64_hex(rc);
    sys_puts("\n");

    uint8_t rnd[16];
    for (uint64_t i = 0; i < sizeof(rnd); i++) rnd[i] = 0;
    rc = sys_getrandom(rnd, sizeof(rnd), 0);
    sys_puts("getrandom rc=");
    write_u64_dec(rc);
    sys_puts(" bytes=");
    for (uint64_t i = 0; i < sizeof(rnd); i++) {
        char b[2];
        const char *h = "0123456789abcdef";
        b[0] = h[(rnd[i] >> 4) & 0xF];
        b[1] = h[rnd[i] & 0xF];
        (void)sys_write(1, b, 2);
    }
    sys_puts("\n");

    sys_puts("compat: OK\n");
    return 0;
}
