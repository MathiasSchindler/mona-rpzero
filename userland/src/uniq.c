#include "syscall.h"

#define AT_FDCWD ((long)-100)

static void putc1(char c) {
    (void)sys_write(1, &c, 1);
}

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

static int cstr_eq_local(const char *a, const char *b) {
    uint64_t i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == b[i];
}

static void u64_to_dec(char *out, uint64_t cap, uint64_t v) {
    if (cap == 0) return;

    char tmp[32];
    uint64_t n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v != 0 && n + 1 < sizeof(tmp)) {
            uint64_t q = v / 10u;
            uint64_t r = v - q * 10u;
            tmp[n++] = (char)('0' + (char)r);
            v = q;
        }
    }

    uint64_t o = 0;
    while (n > 0 && o + 1 < cap) {
        out[o++] = tmp[--n];
    }
    out[o] = '\0';
}

static void print_count_prefix(uint64_t count) {
    char buf[32];
    u64_to_dec(buf, sizeof(buf), count);
    sys_puts(buf);
    sys_puts(" ");
}

static void usage(void) {
    sys_puts("usage: uniq [-c] [-d] [-u] [INPUT]\n");
}

static int read_line(uint64_t fd, char *line, uint64_t cap, int *out_eof) {
    if (!line || cap == 0 || !out_eof) return -1;

    uint64_t n = 0;
    *out_eof = 0;

    for (;;) {
        char c = 0;
        long rc = (long)sys_read(fd, &c, 1);
        if (rc == 0) {
            *out_eof = 1;
            line[n] = '\0';
            return (int)n;
        }
        if (rc < 0) {
            /* EAGAIN (11) => retry (pipes). */
            if (rc == -11) continue;
            return -1;
        }

        if (c == '\r') {
            /* normalize CRLF-ish inputs */
            continue;
        }

        if (c == '\n') {
            line[n] = '\0';
            return (int)n;
        }

        if (n + 1 < cap) {
            line[n++] = c;
        }
        /* If line is too long for the buffer, we truncate (still consume input). */
    }
}

static void emit_line(const char *line, uint64_t count, int opt_c) {
    if (opt_c) {
        print_count_prefix(count);
    }
    sys_puts(line);
    putc1('\n');
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    int opt_c = 0;
    int opt_d = 0;
    int opt_u = 0;

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

        /* Allow combined flags like -cd */
        for (int j = 1; a[j]; j++) {
            char f = a[j];
            if (f == 'c') {
                opt_c = 1;
            } else if (f == 'd') {
                opt_d = 1;
            } else if (f == 'u') {
                opt_u = 1;
            } else {
                usage();
                return 2;
            }
        }
    }

    if (opt_d && opt_u) {
        sys_puts("uniq: cannot combine -d and -u\n");
        return 2;
    }

    int nargs = argc - i;
    if (nargs > 1) {
        sys_puts("uniq: output file not supported\n");
        usage();
        return 2;
    }

    uint64_t fd = 0;
    if (nargs == 1) {
        const char *path = argv[i];
        long opened = (long)sys_openat((uint64_t)AT_FDCWD, path, 0, 0);
        if (opened < 0) {
            sys_puts("uniq: cannot open: ");
            sys_puts(path);
            sys_puts("\n");
            return 1;
        }
        fd = (uint64_t)opened;
    }

    char prev[512];
    char cur[512];
    prev[0] = '\0';

    int eof = 0;
    int n = read_line(fd, prev, sizeof(prev), &eof);
    if (n < 0) {
        sys_puts("uniq: read failed\n");
        if (nargs == 1) (void)sys_close(fd);
        return 1;
    }

    if (eof && prev[0] == '\0') {
        if (nargs == 1) (void)sys_close(fd);
        return 0;
    }

    uint64_t run_count = 1;

    for (;;) {
        eof = 0;
        int m = read_line(fd, cur, sizeof(cur), &eof);
        if (m < 0) {
            sys_puts("uniq: read failed\n");
            if (nargs == 1) (void)sys_close(fd);
            return 1;
        }

        int same = (!eof) && cstr_eq_local(prev, cur);
        if (same) {
            run_count++;
        }

        if (!same) {
            int should_print = 0;
            if (opt_d) {
                should_print = (run_count > 1);
            } else if (opt_u) {
                should_print = (run_count == 1);
            } else {
                should_print = 1;
            }

            if (should_print) {
                emit_line(prev, run_count, opt_c);
            }

            if (eof) break;

            /* Start next run */
            uint64_t k = 0;
            while (cur[k] && k + 1 < sizeof(prev)) {
                prev[k] = cur[k];
                k++;
            }
            prev[k] = '\0';
            run_count = 1;
        }

        if (eof) break;
    }

    if (nargs == 1) (void)sys_close(fd);
    return 0;
}
