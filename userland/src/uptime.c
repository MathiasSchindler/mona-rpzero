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

static void put_u64_dec_pad2(uint64_t v) {
    putc1((char)('0' + (char)((v / 10u) % 10u)));
    putc1((char)('0' + (char)(v % 10u)));
}

static void put_u64_dec_pad(uint64_t v, uint64_t width) {
    char tmp[32];
    uint64_t t = 0;

    if (v == 0) {
        tmp[t++] = '0';
    } else {
        while (v != 0 && t < sizeof(tmp)) {
            tmp[t++] = (char)('0' + (v % 10u));
            v /= 10u;
        }
    }

    while (t < width && t + 1 < sizeof(tmp)) {
        tmp[t++] = '0';
    }
    while (t > 0) {
        putc1(tmp[--t]);
    }
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    linux_timespec_t ts;
    uint64_t rc = sys_clock_gettime(1, &ts); /* CLOCK_MONOTONIC */
    if ((int64_t)rc < 0) {
        sys_puts("uptime: clock_gettime failed\n");
        return 1;
    }

    uint64_t sec = (uint64_t)ts.tv_sec;
    uint64_t nsec = (uint64_t)ts.tv_nsec;

    /* Print "up HH:MM:SS (<sec>.<millis>s)". */
    uint64_t hh = sec / 3600ull;
    uint64_t mm = (sec / 60ull) % 60ull;
    uint64_t ss = sec % 60ull;

    sys_puts("up ");
    put_u64_dec(hh);
    putc1(':');
    put_u64_dec_pad2(mm);
    putc1(':');
    put_u64_dec_pad2(ss);
    sys_puts(" (");
    put_u64_dec(sec);
    putc1('.');
    put_u64_dec_pad(nsec / 1000000ull, 3);
    sys_puts("s)\n");
    return 0;
}
