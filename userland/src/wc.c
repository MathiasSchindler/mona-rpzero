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

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
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

static void print_count(uint64_t v) {
    char buf[32];
    u64_to_dec(buf, sizeof(buf), v);
    sys_puts(buf);
}

static void usage(void) {
    sys_puts("usage: wc [-l] [-w] [-c] [FILE...]\n");
}

typedef struct {
    uint64_t lines;
    uint64_t words;
    uint64_t bytes;
} counts_t;

static int count_fd(uint64_t fd, counts_t *out) {
    counts_t c;
    c.lines = 0;
    c.words = 0;
    c.bytes = 0;

    char buf[512];
    int in_word = 0;

    for (;;) {
        long n = (long)sys_read(fd, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) {
            /* EAGAIN (11) => retry (pipes). */
            if (n == -11) continue;
            return -1;
        }

        c.bytes += (uint64_t)n;

        for (long i = 0; i < n; i++) {
            char ch = buf[i];
            if (ch == '\n') c.lines++;

            if (is_space(ch)) {
                in_word = 0;
            } else {
                if (!in_word) {
                    c.words++;
                    in_word = 1;
                }
            }
        }
    }

    *out = c;
    return 0;
}

static void print_row(int show_lines, int show_words, int show_bytes, const counts_t *c, const char *name) {
    int first = 1;
    if (show_lines) {
        if (!first) putc1(' ');
        first = 0;
        print_count(c->lines);
    }
    if (show_words) {
        if (!first) putc1(' ');
        first = 0;
        print_count(c->words);
    }
    if (show_bytes) {
        if (!first) putc1(' ');
        first = 0;
        print_count(c->bytes);
    }
    if (name) {
        putc1(' ');
        sys_puts(name);
    }
    putc1('\n');
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    int show_lines = 0;
    int show_words = 0;
    int show_bytes = 0;

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

        /* Allow combined flags like -lcw */
        for (int j = 1; a[j]; j++) {
            char f = a[j];
            if (f == 'l') {
                show_lines = 1;
            } else if (f == 'w') {
                show_words = 1;
            } else if (f == 'c') {
                show_bytes = 1;
            } else {
                usage();
                return 2;
            }
        }
    }

    if (!show_lines && !show_words && !show_bytes) {
        show_lines = 1;
        show_words = 1;
        show_bytes = 1;
    }

    int nfiles = argc - i;
    counts_t total;
    total.lines = 0;
    total.words = 0;
    total.bytes = 0;

    if (nfiles <= 0) {
        counts_t c;
        if (count_fd(0, &c) != 0) {
            sys_puts("wc: read failed\n");
            return 1;
        }
        print_row(show_lines, show_words, show_bytes, &c, 0);
        return 0;
    }

    int status = 0;
    for (int fi = 0; fi < nfiles; fi++) {
        const char *path = argv[i + fi];
        long fd = (long)sys_openat((uint64_t)AT_FDCWD, path, 0, 0);
        if (fd < 0) {
            sys_puts("wc: cannot open: ");
            sys_puts(path);
            sys_puts("\n");
            status = 1;
            continue;
        }

        counts_t c;
        if (count_fd((uint64_t)fd, &c) != 0) {
            sys_puts("wc: read failed: ");
            sys_puts(path);
            sys_puts("\n");
            (void)sys_close((uint64_t)fd);
            status = 1;
            continue;
        }
        (void)sys_close((uint64_t)fd);

        total.lines += c.lines;
        total.words += c.words;
        total.bytes += c.bytes;

        print_row(show_lines, show_words, show_bytes, &c, path);
    }

    if (nfiles > 1) {
        print_row(show_lines, show_words, show_bytes, &total, "total");
    }

    return status;
}
