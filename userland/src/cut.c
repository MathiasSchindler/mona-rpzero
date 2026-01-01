#include "syscall.h"

#define AT_FDCWD ((long)-100)

enum {
    O_RDONLY = 0,
};

typedef struct {
    uint64_t start; /* 1-based */
    uint64_t end;   /* inclusive, ignored if end_open */
    int end_open;
} range_t;

static int streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static void usage(void) {
    sys_puts("usage: cut (-c LIST | -f LIST) [-d DELIM] [-s] [FILE...]\n");
    sys_puts("  -c LIST   select character positions (1-based)\n");
    sys_puts("  -f LIST   select fields (1-based)\n");
    sys_puts("  -d DELIM  field delimiter (default: tab)\n");
    sys_puts("  -s        suppress lines with no delimiter (field mode)\n");
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int parse_uint_at(const char *s, uint64_t *i_inout, uint64_t *out) {
    uint64_t i = *i_inout;
    if (!s || !out) return -1;
    if (!is_digit(s[i])) return -1;

    uint64_t v = 0;
    while (is_digit(s[i])) {
        v = v * 10u + (uint64_t)(s[i] - '0');
        i++;
    }

    *i_inout = i;
    *out = v;
    return 0;
}

static int parse_list(const char *s, range_t *out, int out_cap, int *out_n) {
    if (!s || !out || !out_n) return -1;
    *out_n = 0;

    uint64_t i = 0;
    while (s[i] != '\0') {
        if (s[i] == ',') {
            i++;
            continue;
        }

        if (*out_n >= out_cap) return -1;

        range_t r;
        r.start = 0;
        r.end = 0;
        r.end_open = 0;

        if (s[i] == '-') {
            /* -M */
            i++;
            r.start = 1;
            if (parse_uint_at(s, &i, &r.end) != 0 || r.end == 0) return -1;
            r.end_open = 0;
        } else {
            /* N or N-M or N- */
            if (parse_uint_at(s, &i, &r.start) != 0 || r.start == 0) return -1;
            if (s[i] == '-') {
                i++;
                if (s[i] == '\0' || s[i] == ',') {
                    r.end_open = 1;
                    r.end = 0;
                } else {
                    if (parse_uint_at(s, &i, &r.end) != 0 || r.end == 0) return -1;
                    r.end_open = 0;
                }
            } else {
                r.end = r.start;
                r.end_open = 0;
            }
        }

        if (!r.end_open && r.end < r.start) return -1;

        out[(*out_n)++] = r;

        if (s[i] == ',') {
            i++;
            continue;
        }

        if (s[i] == '\0') break;

        /* junk */
        return -1;
    }

    return (*out_n > 0) ? 0 : -1;
}

static int in_ranges(uint64_t pos1, const range_t *rs, int nrs) {
    for (int i = 0; i < nrs; i++) {
        range_t r = rs[i];
        if (pos1 < r.start) continue;
        if (r.end_open) return 1;
        if (pos1 <= r.end) return 1;
    }
    return 0;
}

static int cut_line_chars(const char *line, uint64_t len, const range_t *rs, int nrs) {
    char out[512];
    uint64_t o = 0;

    for (uint64_t i = 0; i < len; i++) {
        uint64_t pos1 = i + 1;
        if (!in_ranges(pos1, rs, nrs)) continue;

        if (o == sizeof(out)) {
            (void)sys_write(1, out, o);
            o = 0;
        }
        out[o++] = line[i];
    }

    if (o) (void)sys_write(1, out, o);
    (void)sys_write(1, "\n", 1);
    return 0;
}

static int cut_line_fields(const char *line, uint64_t len, char delim, int suppress_no_delim, const range_t *rs, int nrs) {
    int has_delim = 0;
    for (uint64_t i = 0; i < len; i++) {
        if (line[i] == delim) {
            has_delim = 1;
            break;
        }
    }

    if (!has_delim) {
        if (suppress_no_delim) return 0;
        (void)sys_write(1, line, len);
        (void)sys_write(1, "\n", 1);
        return 0;
    }

    uint64_t field_idx = 1;
    uint64_t start = 0;
    int first_out = 1;

    for (uint64_t i = 0; i <= len; i++) {
        if (i == len || line[i] == delim) {
            uint64_t flen = i - start;
            if (in_ranges(field_idx, rs, nrs)) {
                if (!first_out) (void)sys_write(1, &delim, 1);
                if (flen) (void)sys_write(1, line + start, flen);
                first_out = 0;
            }
            field_idx++;
            start = i + 1;
        }
    }

    (void)sys_write(1, "\n", 1);
    return 0;
}

static int cut_fd(uint64_t fd,
                  int mode_chars,
                  const range_t *rs,
                  int nrs,
                  char delim,
                  int suppress_no_delim) {
    enum {
        READ_BUF = 512,
        LINE_MAX = 512,
    };

    char rbuf[READ_BUF];
    char line[LINE_MAX];
    uint64_t line_len = 0;
    int line_trunc = 0;

    for (;;) {
        long nread = (long)sys_read(fd, rbuf, sizeof(rbuf));
        if (nread == 0) break;
        if (nread < 0) {
            if (nread == -11) continue; /* EAGAIN */
            return -1;
        }

        for (long i = 0; i < nread; i++) {
            char ch = rbuf[i];
            if (ch == '\n') {
                if (mode_chars) {
                    (void)cut_line_chars(line, line_len, rs, nrs);
                } else {
                    (void)cut_line_fields(line, line_len, delim, suppress_no_delim, rs, nrs);
                }
                line_len = 0;
                line_trunc = 0;
                continue;
            }

            if (!line_trunc) {
                if (line_len + 1 < (uint64_t)sizeof(line)) {
                    line[line_len++] = ch;
                } else {
                    line_trunc = 1;
                }
            }
        }
    }

    if (line_len > 0 || line_trunc) {
        if (mode_chars) {
            (void)cut_line_chars(line, line_len, rs, nrs);
        } else {
            (void)cut_line_fields(line, line_len, delim, suppress_no_delim, rs, nrs);
        }
    }

    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    int mode_chars = 0;
    int mode_fields = 0;
    const char *list_s = 0;
    char delim = '\t';
    int suppress_no_delim = 0;

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
        if (streq(a, "-c")) {
            if (i + 1 >= argc) {
                usage();
                return 2;
            }
            mode_chars = 1;
            mode_fields = 0;
            list_s = argv[++i];
            continue;
        }
        if (streq(a, "-f")) {
            if (i + 1 >= argc) {
                usage();
                return 2;
            }
            mode_fields = 1;
            mode_chars = 0;
            list_s = argv[++i];
            continue;
        }
        if (streq(a, "-d")) {
            if (i + 1 >= argc) {
                usage();
                return 2;
            }
            const char *d = argv[++i];
            if (!d || d[0] == '\0' || d[1] != '\0') {
                sys_puts("cut: -d expects a single character\n");
                return 2;
            }
            delim = d[0];
            continue;
        }
        if (streq(a, "-s")) {
            suppress_no_delim = 1;
            continue;
        }

        usage();
        return 2;
    }

    if (!list_s || (!mode_chars && !mode_fields)) {
        usage();
        return 2;
    }

    range_t ranges[32];
    int nr = 0;
    if (parse_list(list_s, ranges, (int)(sizeof(ranges) / sizeof(ranges[0])), &nr) != 0) {
        sys_puts("cut: invalid LIST\n");
        return 2;
    }

    int nfiles = argc - i;
    if (nfiles <= 0) {
        if (cut_fd(0, mode_chars, ranges, nr, delim, suppress_no_delim) != 0) {
            sys_puts("cut: read failed\n");
            return 1;
        }
        return 0;
    }

    int status = 0;
    for (int fi = 0; fi < nfiles; fi++) {
        const char *path = argv[i + fi];
        if (!path) continue;

        uint64_t fd = 0;
        if (path[0] == '-' && path[1] == '\0') {
            fd = 0;
        } else {
            int64_t rc = (int64_t)sys_openat((uint64_t)AT_FDCWD, path, O_RDONLY, 0);
            if (rc < 0) {
                sys_puts("cut: cannot open: ");
                sys_puts(path);
                sys_puts("\n");
                status = 1;
                continue;
            }
            fd = (uint64_t)rc;
        }

        if (cut_fd(fd, mode_chars, ranges, nr, delim, suppress_no_delim) != 0) {
            sys_puts("cut: read failed: ");
            sys_puts(path);
            sys_puts("\n");
            status = 1;
        }

        if (!(path[0] == '-' && path[1] == '\0')) {
            (void)sys_close(fd);
        }
    }

    return status;
}
