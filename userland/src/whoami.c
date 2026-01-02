#include "syscall.h"

static void putc1(char c) {
    (void)sys_write(1, &c, 1);
}

static void put_u64_dec(uint64_t v) {
    char tmp[32];
    uint64_t t = 0;

    if (v == 0) {
        putc1('0');
        return;
    }
    while (v != 0 && t < sizeof(tmp)) {
        tmp[t++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (t > 0) {
        putc1(tmp[--t]);
    }
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    uint64_t uid = sys_getuid();
    if (uid == 0) {
        sys_puts("root\n");
        return 0;
    }

    /* No user database yet; emit a stable synthetic name. */
    sys_puts("uid");
    put_u64_dec(uid);
    putc1('\n');
    return 0;
}
