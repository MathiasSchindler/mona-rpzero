#include "syscall.h"

#define AT_FDCWD ((long)-100)

static int streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static void usage(void) {
    sys_puts("usage: head [-n LINES] [-c BYTES] [FILE...]\n");
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static uint64_t parse_u64(const char *s, int *ok) {
    if (!ok) return 0;
    *ok = 0;
    if (!s || *s == '\0') return 0;

    uint64_t v = 0;
    for (uint64_t i = 0; s[i] != '\0'; i++) {
        if (!is_digit(s[i])) return 0;
        uint64_t d = (uint64_t)(s[i] - '0');
        uint64_t nv = v * 10u + d;
        if (nv < v) return 0;
        v = nv;
    }

    *ok = 1;
    return v;
}

static int head_bytes_fd(uint64_t fd, uint64_t max_bytes) {
    char buf[512];
    while (max_bytes > 0) {
        uint64_t want = (max_bytes < (uint64_t)sizeof(buf)) ? max_bytes : (uint64_t)sizeof(buf);
        long n = (long)sys_read(fd, buf, want);
        if (n == 0) return 0;
        if (n < 0) {
            /* EAGAIN (11) => retry (pipes). */
            if (n == -11) continue;
            return -1;
        }
        (void)sys_write(1, buf, (uint64_t)n);
        max_bytes -= (uint64_t)n;
    }
    return 0;
}

static int head_lines_fd(uint64_t fd, uint64_t max_lines) {
    if (max_lines == 0) return 0;

    char buf[512];
    uint64_t lines_left = max_lines;

    for (;;) {
        long n = (long)sys_read(fd, buf, sizeof(buf));
        if (n == 0) return 0;
        if (n < 0) {
            if (n == -11) continue;
            return -1;
        }

        uint64_t out_n = 0;
        for (long i = 0; i < n; i++) {
            out_n++;
            if (buf[i] == '\n') {
                if (lines_left > 0) lines_left--;
                if (lines_left == 0) {
                    break;
                }
            }
        }

        (void)sys_write(1, buf, out_n);
        if (lines_left == 0) return 0;
    }
}

static void print_header_if_needed(int show_header, const char *name, int first) {
    if (!show_header) return;
    if (!first) sys_puts("\n");
    sys_puts("==> ");
    sys_puts(name);
    sys_puts(" <==\n");
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    int opt_bytes = 0;
    uint64_t n_lines = 10;
    uint64_t n_bytes = 0;

    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!a || a[0] != '-') break;
        if (streq(a, "--")) {
            i++;
            break;
        }
        if (streq(a, "-h") || streq(a, "--help")) {
            usage();
            return 0;
        }

        if (streq(a, "-n")) {
            if (i + 1 >= argc) {
                usage();
                return 2;
            }
            int ok = 0;
            uint64_t v = parse_u64(argv[++i], &ok);
            if (!ok) {
                sys_puts("head: invalid -n\n");
                return 2;
            }
            opt_bytes = 0;
            n_lines = v;
            continue;
        }
        if (a[0] == '-' && a[1] == 'n' && a[2] != '\0') {
            int ok = 0;
            uint64_t v = parse_u64(a + 2, &ok);
            if (!ok) {
                sys_puts("head: invalid -n\n");
                return 2;
            }
            opt_bytes = 0;
            n_lines = v;
            continue;
        }

        if (streq(a, "-c")) {
            if (i + 1 >= argc) {
                usage();
                return 2;
            }
            int ok = 0;
            uint64_t v = parse_u64(argv[++i], &ok);
            if (!ok) {
                sys_puts("head: invalid -c\n");
                return 2;
            }
            opt_bytes = 1;
            n_bytes = v;
            continue;
        }
        if (a[0] == '-' && a[1] == 'c' && a[2] != '\0') {
            int ok = 0;
            uint64_t v = parse_u64(a + 2, &ok);
            if (!ok) {
                sys_puts("head: invalid -c\n");
                return 2;
            }
            opt_bytes = 1;
            n_bytes = v;
            continue;
        }

        usage();
        return 2;
    }

    int nfiles = argc - i;
    int show_header = (nfiles > 1);

    if (nfiles <= 0) {
        if (opt_bytes) {
            if (head_bytes_fd(0, n_bytes) != 0) {
                sys_puts("head: read failed\n");
                return 1;
            }
        } else {
            if (head_lines_fd(0, n_lines) != 0) {
                sys_puts("head: read failed\n");
                return 1;
            }
        }
        return 0;
    }

    int status = 0;
    for (int fi = 0; fi < nfiles; fi++) {
        const char *path = argv[i + fi];
        print_header_if_needed(show_header, path, fi == 0);

        long fd = (long)sys_openat((uint64_t)AT_FDCWD, path, 0, 0);
        if (fd < 0) {
            sys_puts("head: cannot open: ");
            sys_puts(path);
            sys_puts("\n");
            status = 1;
            continue;
        }

        int rc = 0;
        if (opt_bytes) rc = head_bytes_fd((uint64_t)fd, n_bytes);
        else rc = head_lines_fd((uint64_t)fd, n_lines);

        (void)sys_close((uint64_t)fd);
        if (rc != 0) {
            sys_puts("head: read failed: ");
            sys_puts(path);
            sys_puts("\n");
            status = 1;
        }
    }

    return status;
}
