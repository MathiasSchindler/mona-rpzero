#include "syscall.h"

#define AT_FDCWD ((long)-100)

enum {
    LINUX_EEXIST = 17,
    LINUX_ENAMETOOLONG = 36,
};

static int streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static int parse_octal_mode(const char *s, uint64_t *out_mode) {
    if (!s || s[0] == '\0') return -1;

    uint64_t v = 0;
    int digits = 0;
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (c < '0' || c > '7') return -1;
        v = (v << 3) + (uint64_t)(c - '0');
        digits++;
        if (digits > 6) return -1;
    }

    *out_mode = v;
    return 0;
}

static void usage(void) {
    sys_puts("usage: mkdir [-p] [-m MODE] DIR...\n");
}

static long mkdir_one(const char *path, uint64_t mode) {
    return (long)sys_mkdirat((uint64_t)AT_FDCWD, path, mode);
}

static long mkdir_p(const char *path, uint64_t mode) {
    if (!path || path[0] == '\0') return -1;

    char buf[512];
    int bi = 0;

    int i = 0;
    if (path[0] == '/') {
        buf[bi++] = '/';
        i = 1;
    }

    while (path[i]) {
        while (path[i] == '/') i++;
        if (!path[i]) break;

        if (bi > 0 && buf[bi - 1] != '/') {
            if (bi + 1 >= (int)sizeof(buf)) return -LINUX_ENAMETOOLONG;
            buf[bi++] = '/';
        }

        while (path[i] && path[i] != '/') {
            if (bi + 1 >= (int)sizeof(buf)) return -LINUX_ENAMETOOLONG;
            buf[bi++] = path[i++];
        }
        buf[bi] = '\0';

        long rc = mkdir_one(buf, mode);
        if (rc < 0 && rc != -LINUX_EEXIST) {
            return rc;
        }
    }

    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    int opt_p = 0;
    uint64_t mode = 0777;

    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!a || a[0] == '\0') break;

        if (a[0] != '-') break;
        if (streq(a, "--")) {
            i++;
            break;
        }

        if (streq(a, "-p")) {
            opt_p = 1;
            continue;
        }

        if (streq(a, "-m")) {
            if (i + 1 >= argc) {
                sys_puts("mkdir: -m requires MODE\n");
                usage();
                return 2;
            }
            if (parse_octal_mode(argv[i + 1], &mode) != 0) {
                sys_puts("mkdir: invalid MODE\n");
                usage();
                return 2;
            }
            i++;
            continue;
        }

        sys_puts("mkdir: unknown option\n");
        usage();
        return 2;
    }

    if (i >= argc) {
        usage();
        return 1;
    }

    int status = 0;
    for (; i < argc; i++) {
        const char *path = argv[i];
        if (!path || path[0] == '\0') continue;

        long rc = opt_p ? mkdir_p(path, mode) : mkdir_one(path, mode);
        if (rc < 0) {
            sys_puts("mkdir: failed: ");
            sys_puts(path);
            sys_puts("\n");
            status = 1;
        }
    }

    return status;
}
