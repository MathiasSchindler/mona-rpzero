#include "syscall.h"

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
    sys_puts("usage: who [--help]\n");
    sys_puts("notes: no utmp yet; prints a single best-effort line.\n");
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!a) continue;
        if (streq(a, "--help") || streq(a, "-h")) {
            usage();
            return 0;
        }
        sys_puts("who: unsupported arg: '");
        sys_puts(a);
        sys_puts("'\n");
        usage();
        return 2;
    }

    /* Minimal compatibility: report one session on the system console. */
    uint64_t uid = sys_getuid();
    if (uid == 0) {
        sys_puts("root console\n");
    } else {
        sys_puts("uid");
        /* tiny decimal */
        char tmp[32];
        uint64_t t = 0;
        uint64_t v = uid;
        if (v == 0) {
            tmp[t++] = '0';
        } else {
            while (v != 0 && t < sizeof(tmp)) {
                tmp[t++] = (char)('0' + (v % 10u));
                v /= 10u;
            }
        }
        while (t > 0) {
            char c = tmp[--t];
            (void)sys_write(1, &c, 1);
        }
        sys_puts(" console\n");
    }

    return 0;
}
