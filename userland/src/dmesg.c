#include "syscall.h"

enum { DMESG_F_CLEAR = 1u };

static int streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static void usage(void) {
    sys_puts("usage: dmesg [-c|-C]\n");
    sys_puts("  -c  print and clear\n");
    sys_puts("  -C  clear only\n");
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    int clear = 0;
    int clear_only = 0;

    for (int i = 1; i < argc; i++) {
        if (streq(argv[i], "-c")) {
            clear = 1;
        } else if (streq(argv[i], "-C")) {
            clear_only = 1;
        } else if (streq(argv[i], "-h") || streq(argv[i], "--help")) {
            usage();
            return 0;
        } else {
            usage();
            return 1;
        }
    }

    if (clear_only) {
        (void)sys_mona_dmesg(0, 0, DMESG_F_CLEAR);
        return 0;
    }

    /* Kernel-side buffer is small; use a fixed static buffer. */
    static char buf[64 * 1024 + 1];

    uint64_t n = sys_mona_dmesg(buf, (uint64_t)(sizeof(buf) - 1u), clear ? DMESG_F_CLEAR : 0);
    if ((int64_t)n < 0) {
        sys_puts("dmesg: read failed\n");
        return 1;
    }

    if (n == 0) {
        return 0;
    }

    (void)sys_write(1, buf, n);
    return 0;
}
