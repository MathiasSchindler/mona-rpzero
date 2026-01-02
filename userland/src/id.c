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

static void usage(void) {
    sys_puts("usage: id [-u|-g] [--help]\n");
    sys_puts("notes: prints numeric IDs only (no user/group names).\n");
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    int want_u = 0;
    int want_g = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!a) continue;

        if (streq(a, "--help") || streq(a, "-h")) {
            usage();
            return 0;
        }
        if (streq(a, "-u")) {
            want_u = 1;
            continue;
        }
        if (streq(a, "-g")) {
            want_g = 1;
            continue;
        }

        sys_puts("id: unsupported arg: '");
        sys_puts(a);
        sys_puts("'\n");
        usage();
        return 2;
    }

    uint64_t uid = sys_getuid();
    uint64_t gid = sys_getgid();

    /* Match common behavior: -u/-g output just the number. */
    if (want_u && !want_g) {
        put_u64_dec(uid);
        putc1('\n');
        return 0;
    }
    if (want_g && !want_u) {
        put_u64_dec(gid);
        putc1('\n');
        return 0;
    }

    /* Default (or both requested): concise numeric print. */
    sys_puts("uid=");
    put_u64_dec(uid);
    sys_puts(" gid=");
    put_u64_dec(gid);
    putc1('\n');
    return 0;
}
