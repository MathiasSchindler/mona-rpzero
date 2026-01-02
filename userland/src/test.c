#include "syscall.h"

#define AT_FDCWD ((int64_t)-100)

static int streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static uint64_t cstr_len_u64_local(const char *s) {
    uint64_t n = 0;
    while (s && s[n] != '\0') n++;
    return n;
}

static int is_reg(uint32_t mode) {
    return (mode & 0170000u) == 0100000u;
}

static int is_dir(uint32_t mode) {
    return (mode & 0170000u) == 0040000u;
}

static int file_test(const char *path, int want_exists, int want_reg, int want_dir) {
    linux_stat_t st;
    int64_t rc = (int64_t)sys_newfstatat((uint64_t)AT_FDCWD, path, &st, 0);
    if (rc < 0) return 0;
    if (want_exists) return 1;
    if (want_reg) return is_reg(st.st_mode);
    if (want_dir) return is_dir(st.st_mode);
    return 0;
}

static void usage(void) {
    sys_puts("usage: test EXPR\n");
    sys_puts("  Supported:\n");
    sys_puts("    test STRING\n");
    sys_puts("    test -z STRING\n");
    sys_puts("    test -n STRING\n");
    sys_puts("    test -e FILE\n");
    sys_puts("    test -f FILE\n");
    sys_puts("    test -d FILE\n");
    sys_puts("    test STRING1 = STRING2\n");
    sys_puts("    test STRING1 != STRING2\n");
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    if (argc <= 1) {
        return 1;
    }

    if (argc == 2) {
        /* Non-empty string is true. */
        return (argv[1] && argv[1][0] != '\0') ? 0 : 1;
    }

    if (argc == 3) {
        const char *op = argv[1];
        const char *arg = argv[2] ? argv[2] : "";

        if (streq(op, "-z")) {
            return (cstr_len_u64_local(arg) == 0) ? 0 : 1;
        }
        if (streq(op, "-n")) {
            return (cstr_len_u64_local(arg) != 0) ? 0 : 1;
        }
        if (streq(op, "-e")) {
            return file_test(arg, 1, 0, 0) ? 0 : 1;
        }
        if (streq(op, "-f")) {
            return file_test(arg, 0, 1, 0) ? 0 : 1;
        }
        if (streq(op, "-d")) {
            return file_test(arg, 0, 0, 1) ? 0 : 1;
        }

        if (streq(op, "-h") || streq(op, "--help")) {
            usage();
            return 0;
        }

        usage();
        return 2;
    }

    if (argc == 4) {
        const char *a = argv[1] ? argv[1] : "";
        const char *op = argv[2] ? argv[2] : "";
        const char *b = argv[3] ? argv[3] : "";

        if (streq(op, "=")) {
            return streq(a, b) ? 0 : 1;
        }
        if (streq(op, "!=")) {
            return streq(a, b) ? 1 : 0;
        }

        usage();
        return 2;
    }

    usage();
    return 2;
}
