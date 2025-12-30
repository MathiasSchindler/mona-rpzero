#include "syscall.h"

#define AT_FDCWD ((int64_t)-100)

/* getdents64 d_type values (subset) */
#define LINUX_DT_UNKNOWN 0
#define LINUX_DT_DIR 4

/* st_mode file types (POSIX) */
#define S_IFMT 0170000u
#define S_IFDIR 0040000u
#define S_IFREG 0100000u

typedef struct {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[];
} linux_dirent64_t;

enum {
    MAX_PATH = 256,
    DENTS_BUF = 512,
};

static int streq(const char *a, const char *b) {
    if (!a || !b) return 0;
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

static void puts1(const char *s) {
    (void)sys_write(1, s, cstr_len_u64_local(s));
}

static void putc1(char c) {
    (void)sys_write(1, &c, 1);
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int parse_i32(const char *s, int *out) {
    if (!s || !out) return -1;
    int sign = 1;
    if (*s == '+') s++;
    if (*s == '-') {
        sign = -1;
        s++;
    }
    if (!is_digit(*s)) return -1;
    int v = 0;
    while (*s && is_digit(*s)) {
        v = v * 10 + (*s - '0');
        s++;
    }
    *out = v * sign;
    return 0;
}

static int join_path(char *out, uint64_t cap, const char *base, const char *name) {
    if (!out || cap == 0 || !base || !name) return -1;
    uint64_t bl = cstr_len_u64_local(base);
    uint64_t nl = cstr_len_u64_local(name);
    if (bl == 0) return -1;

    int need_slash = 1;
    if (base[bl - 1] == '/') need_slash = 0;

    uint64_t total = bl + (uint64_t)need_slash + nl;
    if (total + 1 > cap) return -1;

    uint64_t o = 0;
    for (uint64_t i = 0; i < bl; i++) out[o++] = base[i];
    if (need_slash) out[o++] = '/';
    for (uint64_t i = 0; i < nl; i++) out[o++] = name[i];
    out[o] = '\0';
    return 0;
}

static const char *basename_ptr(const char *path) {
    if (!path) return "";
    const char *last = path;
    for (uint64_t i = 0; path[i] != '\0'; i++) {
        if (path[i] == '/' && path[i + 1] != '\0') last = &path[i + 1];
    }
    return last;
}

static int match_glob(const char *pat, const char *s) {
    if (!pat || !s) return 0;

    /* Simple glob: '*' matches any sequence, '?' matches one char. */
    const char *p = pat;
    const char *t = s;
    const char *star = 0;
    const char *star_t = 0;

    while (*t) {
        if (*p == '*') {
            star = p++;
            star_t = t;
            continue;
        }
        if (*p == '?' || *p == *t) {
            p++;
            t++;
            continue;
        }
        if (star) {
            p = star + 1;
            t = ++star_t;
            continue;
        }
        return 0;
    }

    while (*p == '*') p++;
    return *p == '\0';
}

static int g_type_filter = 0; /* 0 none, 'f', 'd' */
static const char *g_name_pat = 0;
static int g_maxdepth = 64;
static int g_mindepth = 0;

static int mode_is_dir(uint32_t mode) {
    return (mode & S_IFMT) == S_IFDIR;
}

static int mode_is_reg(uint32_t mode) {
    return (mode & S_IFMT) == S_IFREG;
}

static int matches_filters(const char *path, const linux_stat_t *st, int depth) {
    if (!path || !st) return 0;

    if (depth < g_mindepth) return 0;

    if (g_type_filter == 'd' && !mode_is_dir(st->st_mode)) return 0;
    if (g_type_filter == 'f' && !mode_is_reg(st->st_mode)) return 0;

    if (g_name_pat) {
        const char *bn = basename_ptr(path);
        if (!match_glob(g_name_pat, bn)) return 0;
    }

    return 1;
}

static int walk(const char *path, int depth) {
    linux_stat_t st;
    uint64_t rc = sys_newfstatat((uint64_t)AT_FDCWD, path, &st, 0);
    if ((int64_t)rc < 0) {
        /* Keep find usable even if some paths can't be stat'ed. */
        return 0;
    }

    if (matches_filters(path, &st, depth)) {
        puts1(path);
        putc1('\n');
    }

    if (!mode_is_dir(st.st_mode)) {
        return 0;
    }

    if (depth >= g_maxdepth) {
        return 0;
    }

    long fd = (long)sys_openat((uint64_t)AT_FDCWD, path, 0, 0);
    if (fd < 0) {
        return 0;
    }

    char buf[DENTS_BUF];
    for (;;) {
        long nread = (long)sys_getdents64((uint64_t)fd, buf, sizeof(buf));
        if (nread < 0) {
            (void)sys_close((uint64_t)fd);
            return 0;
        }
        if (nread == 0) break;

        uint64_t pos = 0;
        while (pos + 19u <= (uint64_t)nread) {
            linux_dirent64_t *de = (linux_dirent64_t *)(void *)(buf + pos);
            if (de->d_reclen == 0) break;
            if (pos + de->d_reclen > (uint64_t)nread) break;

            const char *name = de->d_name;
            if (name && name[0] != '\0' && !streq(name, ".") && !streq(name, "..")) {
                char child[MAX_PATH];
                if (join_path(child, sizeof(child), path, name) == 0) {
                    (void)walk(child, depth + 1);
                }
            }

            pos += de->d_reclen;
        }
    }

    (void)sys_close((uint64_t)fd);
    return 0;
}

static void usage(void) {
    sys_puts("usage: find [path] [-name PAT] [-type f|d] [-maxdepth N] [-mindepth N]\n");
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    const char *path = ".";
    int i = 1;

    if (i < argc && argv[i] && argv[i][0] != '-') {
        path = argv[i];
        i++;
    }

    for (; i < argc; i++) {
        if (streq(argv[i], "-name")) {
            if (i + 1 >= argc) {
                usage();
                return 1;
            }
            g_name_pat = argv[++i];
            continue;
        }

        if (streq(argv[i], "-type")) {
            if (i + 1 >= argc) {
                usage();
                return 1;
            }
            const char *t = argv[++i];
            if (streq(t, "f")) {
                g_type_filter = 'f';
            } else if (streq(t, "d")) {
                g_type_filter = 'd';
            } else {
                usage();
                return 1;
            }
            continue;
        }

        if (streq(argv[i], "-maxdepth")) {
            if (i + 1 >= argc) {
                usage();
                return 1;
            }
            int v = 0;
            if (parse_i32(argv[++i], &v) != 0 || v < 0) {
                usage();
                return 1;
            }
            g_maxdepth = v;
            continue;
        }

        if (streq(argv[i], "-mindepth")) {
            if (i + 1 >= argc) {
                usage();
                return 1;
            }
            int v = 0;
            if (parse_i32(argv[++i], &v) != 0 || v < 0) {
                usage();
                return 1;
            }
            g_mindepth = v;
            continue;
        }

        usage();
        return 1;
    }

    /* Reasonable defaults */
    if (!g_name_pat) g_name_pat = 0;

    (void)walk(path, 0);
    return 0;
}
