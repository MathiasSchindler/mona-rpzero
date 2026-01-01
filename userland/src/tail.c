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
    sys_puts("usage: tail [-n LINES] [-c BYTES] [FILE...]\n");
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

static void print_header_if_needed(int show_header, const char *name, int first) {
    if (!show_header) return;
    if (!first) sys_puts("\n");
    sys_puts("==> ");
    sys_puts(name);
    sys_puts(" <==\n");
}

static int stream_copy_fd(uint64_t fd) {
    char buf[512];
    for (;;) {
        long n = (long)sys_read(fd, buf, sizeof(buf));
        if (n == 0) return 0;
        if (n < 0) {
            if (n == -11) continue;
            return -1;
        }
        (void)sys_write(1, buf, (uint64_t)n);
    }
}

/* File-backed tail for bytes. */
static int tail_bytes_seek_fd(uint64_t fd, int64_t size, uint64_t nbytes) {
    if (nbytes == 0) return 0;

    int64_t start = 0;
    if ((uint64_t)size > nbytes) start = size - (int64_t)nbytes;

    if ((int64_t)sys_lseek(fd, start, 0) < 0) return -1;
    return stream_copy_fd(fd);
}

/* File-backed tail for lines using backwards scan. */
static int tail_lines_seek_fd(uint64_t fd, int64_t size, uint64_t nlines) {
    if (nlines == 0) return 0;
    if (size <= 0) return 0;

    char buf[512];

    int64_t pos = size;
    uint64_t seen = 0;
    int64_t start = 0;

    while (pos > 0 && seen <= nlines) {
        int64_t chunk = (pos > (int64_t)sizeof(buf)) ? (int64_t)sizeof(buf) : pos;
        pos -= chunk;

        if ((int64_t)sys_lseek(fd, pos, 0) < 0) return -1;

        long nr = (long)sys_read(fd, buf, (uint64_t)chunk);
        if (nr < 0) {
            if (nr == -11) continue;
            return -1;
        }
        if (nr == 0) break;

        for (long i = nr - 1; i >= 0; i--) {
            if (buf[i] == '\n') {
                seen++;
                if (seen == nlines + 1) {
                    start = pos + (int64_t)i + 1;
                    pos = 0;
                    break;
                }
            }
        }
    }

    if ((int64_t)sys_lseek(fd, start, 0) < 0) return -1;
    return stream_copy_fd(fd);
}

/* Streaming tail (for stdin / pipes / non-seekable) using a byte ring buffer. */
enum {
    RING_CAP = 65536,
    NL_MAX = 8192,
};

typedef struct {
    char buf[RING_CAP];
    uint64_t head;    /* index of oldest byte */
    uint64_t len;     /* number of bytes stored */
    uint64_t abs_base;/* absolute offset of buf[head] */
} ring_t;

static void ring_init(ring_t *r) {
    r->head = 0;
    r->len = 0;
    r->abs_base = 0;
}

static void ring_push(ring_t *r, char c) {
    if (r->len < (uint64_t)RING_CAP) {
        uint64_t tail = (r->head + r->len) % (uint64_t)RING_CAP;
        r->buf[tail] = c;
        r->len++;
        return;
    }

    /* overwrite oldest */
    r->buf[r->head] = c;
    r->head = (r->head + 1u) % (uint64_t)RING_CAP;
    r->abs_base++;
}

static void ring_write_from_abs(const ring_t *r, uint64_t abs_start) {
    if (r->len == 0) return;

    if (abs_start < r->abs_base) abs_start = r->abs_base;
    uint64_t abs_end = r->abs_base + r->len;
    if (abs_start >= abs_end) return;

    uint64_t rel = abs_start - r->abs_base;
    uint64_t idx = (r->head + rel) % (uint64_t)RING_CAP;
    uint64_t remain = abs_end - abs_start;

    uint64_t first = (uint64_t)RING_CAP - idx;
    if (first > remain) first = remain;

    (void)sys_write(1, &r->buf[idx], first);
    remain -= first;
    if (remain > 0) {
        (void)sys_write(1, &r->buf[0], remain);
    }
}

static int tail_stream_bytes(uint64_t fd, uint64_t nbytes) {
    ring_t r;
    ring_init(&r);

    uint64_t total = 0;
    char buf[512];

    for (;;) {
        long n = (long)sys_read(fd, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) {
            if (n == -11) continue;
            return -1;
        }

        for (long i = 0; i < n; i++) {
            ring_push(&r, buf[i]);
            total++;
        }
    }

    uint64_t start = 0;
    if (nbytes < total) start = total - nbytes;
    ring_write_from_abs(&r, start);
    return 0;
}

static int tail_stream_lines(uint64_t fd, uint64_t nlines) {
    if (nlines == 0) return 0;
    if (nlines >= (uint64_t)(NL_MAX - 1)) {
        sys_puts("tail: -n too large\n");
        return -1;
    }

    ring_t r;
    ring_init(&r);

    uint64_t total = 0;

    uint64_t nl_count = 0;
    uint64_t nl_ring[NL_MAX];

    char buf[512];
    for (;;) {
        long n = (long)sys_read(fd, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) {
            if (n == -11) continue;
            return -1;
        }

        for (long i = 0; i < n; i++) {
            char c = buf[i];
            ring_push(&r, c);
            if (c == '\n') {
                nl_ring[nl_count % (uint64_t)NL_MAX] = total;
                nl_count++;
            }
            total++;
        }
    }

    uint64_t start = 0;
    if (nl_count > nlines) {
        uint64_t idx = (nl_count - nlines - 1u) % (uint64_t)NL_MAX;
        start = nl_ring[idx] + 1u;
    }

    ring_write_from_abs(&r, start);
    return 0;
}

static int tail_fd(uint64_t fd, const char *path, int opt_bytes, uint64_t nbytes, uint64_t nlines) {
    if (opt_bytes) {
        /* try seek path if available */
        if (path) {
            linux_stat_t st;
            if ((int64_t)sys_newfstatat((uint64_t)AT_FDCWD, path, &st, 0) == 0) {
                if (tail_bytes_seek_fd(fd, st.st_size, nbytes) == 0) return 0;
                /* fall through to streaming */
            }
        }
        return tail_stream_bytes(fd, nbytes);
    }

    /* lines */
    if (path) {
        linux_stat_t st;
        if ((int64_t)sys_newfstatat((uint64_t)AT_FDCWD, path, &st, 0) == 0) {
            if (tail_lines_seek_fd(fd, st.st_size, nlines) == 0) return 0;
        }
    }
    return tail_stream_lines(fd, nlines);
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    int opt_bytes = 0;
    uint64_t nlines = 10;
    uint64_t nbytes = 0;

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
                sys_puts("tail: invalid -n\n");
                return 2;
            }
            opt_bytes = 0;
            nlines = v;
            continue;
        }
        if (a[0] == '-' && a[1] == 'n' && a[2] != '\0') {
            int ok = 0;
            uint64_t v = parse_u64(a + 2, &ok);
            if (!ok) {
                sys_puts("tail: invalid -n\n");
                return 2;
            }
            opt_bytes = 0;
            nlines = v;
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
                sys_puts("tail: invalid -c\n");
                return 2;
            }
            opt_bytes = 1;
            nbytes = v;
            continue;
        }
        if (a[0] == '-' && a[1] == 'c' && a[2] != '\0') {
            int ok = 0;
            uint64_t v = parse_u64(a + 2, &ok);
            if (!ok) {
                sys_puts("tail: invalid -c\n");
                return 2;
            }
            opt_bytes = 1;
            nbytes = v;
            continue;
        }

        usage();
        return 2;
    }

    int nfiles = argc - i;
    int show_header = (nfiles > 1);

    if (nfiles <= 0) {
        int rc = tail_fd(0, 0, opt_bytes, nbytes, nlines);
        if (rc != 0) {
            sys_puts("tail: read failed\n");
            return 1;
        }
        return 0;
    }

    int status = 0;
    for (int fi = 0; fi < nfiles; fi++) {
        const char *path = argv[i + fi];
        print_header_if_needed(show_header, path, fi == 0);

        long fd = (long)sys_openat((uint64_t)AT_FDCWD, path, 0, 0);
        if (fd < 0) {
            sys_puts("tail: cannot open: ");
            sys_puts(path);
            sys_puts("\n");
            status = 1;
            continue;
        }

        int rc = tail_fd((uint64_t)fd, path, opt_bytes, nbytes, nlines);
        (void)sys_close((uint64_t)fd);
        if (rc != 0) {
            sys_puts("tail: read failed: ");
            sys_puts(path);
            sys_puts("\n");
            status = 1;
        }
    }

    return status;
}
