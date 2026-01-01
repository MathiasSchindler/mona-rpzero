#include "syscall.h"

#define AT_FDCWD ((int64_t)-100)

static int streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static void write_i64_dec(int64_t v) {
    char buf[32];
    uint64_t n = 0;

    if (v < 0) {
        buf[n++] = '-';
        v = -v;
    }

    if (v == 0) {
        buf[n++] = '0';
    } else {
        char tmp[32];
        uint64_t m = 0;
        uint64_t t = (uint64_t)v;
        while (t > 0 && m < sizeof(tmp)) {
            tmp[m++] = (char)('0' + (t % 10u));
            t /= 10u;
        }
        while (m > 0) {
            buf[n++] = tmp[--m];
        }
    }

    (void)sys_write(1, buf, n);
}

static int parse_mode_octal(const char *s, uint32_t *out_mode) {
    if (!s || !out_mode) return -1;
    if (s[0] == '\0') return -1;

    uint32_t v = 0;
    for (uint64_t i = 0; s[i] != '\0'; i++) {
        char c = s[i];
        if (c < '0' || c > '7') {
            return -1;
        }
        v = (v << 3) + (uint32_t)(c - '0');
        if (v > 07777u) {
            /* Keep it simple: allow up to 4 octal digits (incl. suid/sgid/sticky),
             * but the kernel currently applies only 0777.
             */
            return -1;
        }
    }

    *out_mode = v;
    return 0;
}

static void usage(void) {
    sys_puts("usage: chmod MODE FILE...\n");
    sys_puts("       chmod -h|--help\n");
    sys_puts("note: MODE currently supports octal digits only (e.g. 644, 755).\n");
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    if (argc >= 2 && argv[1] && (streq(argv[1], "-h") || streq(argv[1], "--help"))) {
        usage();
        return 0;
    }

    if (argc < 3) {
        usage();
        return 2;
    }

    uint32_t mode = 0;
    if (parse_mode_octal(argv[1], &mode) != 0) {
        sys_puts("chmod: invalid mode: '");
        sys_puts(argv[1]);
        sys_puts("'\n");
        return 2;
    }

    int status = 0;
    for (int i = 2; i < argc; i++) {
        const char *path = argv[i];
        if (!path || path[0] == '\0') continue;

        int64_t rc = (int64_t)sys_fchmodat(AT_FDCWD, path, (uint64_t)mode, 0);
        if (rc < 0) {
            sys_puts("chmod: fchmodat failed rc=");
            write_i64_dec(rc);
            sys_puts(" path='");
            sys_puts(path);
            sys_puts("'\n");
            status = 1;
        }
    }

    return status;
}
