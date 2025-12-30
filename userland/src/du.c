#include "syscall.h"

#define AT_FDCWD ((int64_t)-100)

/* getdents64 d_type values (subset) */
#define LINUX_DT_UNKNOWN 0
#define LINUX_DT_DIR 4

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
    MAX_DEPTH = 64,
};

static uint64_t cstr_len_u64_local(const char *s) {
    uint64_t n = 0;
    while (s && s[n] != '\0') n++;
    return n;
}

static int streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static void write_u64_dec(uint64_t v) {
    char tmp[32];
    uint64_t t = 0;
    if (v == 0) {
        tmp[t++] = '0';
    } else {
        char rev[32];
        uint64_t r = 0;
        while (v > 0 && r < sizeof(rev)) {
            rev[r++] = (char)('0' + (v % 10));
            v /= 10;
        }
        while (r > 0) tmp[t++] = rev[--r];
    }
    (void)sys_write(1, tmp, t);
}

static void write_size(uint64_t bytes, int human) {
    if (!human) {
        write_u64_dec(bytes);
        return;
    }

    static const char *const suf[] = {"B", "K", "M", "G", "T"};
    uint64_t val = bytes;
    uint64_t rem = 0;
    uint64_t unit = 0;
    while (val >= 1024u && unit + 1 < (uint64_t)(sizeof(suf) / sizeof(suf[0]))) {
        rem = val % 1024u;
        val /= 1024u;
        unit++;
    }

    write_u64_dec(val);
    if (unit > 0) {
        uint64_t frac = (rem * 10u) / 1024u;
        (void)sys_write(1, ".", 1);
        (void)sys_write(1, (char[]){(char)('0' + (char)frac)}, 1);
    }
    sys_puts(suf[unit]);
}

static int join_path(char *out, uint64_t cap, const char *base, const char *name) {
    uint64_t bl = cstr_len_u64_local(base);
    uint64_t nl = cstr_len_u64_local(name);
    if (!base || bl == 0) return -1;
    int need_slash = (base[bl - 1] == '/') ? 0 : 1;
    uint64_t total = bl + (uint64_t)need_slash + nl;
    if (total + 1 > cap) return -1;

    uint64_t o = 0;
    for (uint64_t i = 0; i < bl; i++) out[o++] = base[i];
    if (need_slash) out[o++] = '/';
    for (uint64_t i = 0; i < nl; i++) out[o++] = name[i];
    out[o] = '\0';
    return 0;
}

static uint64_t du_walk(const char *path, int depth, int is_root, int summary_only, int include_files, int human, int *printed_any) {
    if (depth > MAX_DEPTH) {
        return 0;
    }

    linux_stat_t st;
    int64_t src = (int64_t)sys_newfstatat((uint64_t)AT_FDCWD, path, &st, 0);
    if (src < 0) {
        /* Silent failure like many tools; keep tests robust. */
        return 0;
    }

    uint32_t mode = st.st_mode;

    if ((mode & 0170000u) == 0100000u) {
        /* Regular file. */
        uint64_t sz = (uint64_t)st.st_size;
        if (!summary_only && (include_files || is_root)) {
            write_size(sz, human);
            sys_puts("\t");
            sys_puts(path);
            sys_puts("\n");
            if (printed_any) *printed_any = 1;
        }
        return sz;
    }

    if ((mode & 0170000u) != 0040000u) {
        return 0;
    }

    uint64_t dfd = sys_openat((uint64_t)AT_FDCWD, path, 0, 0);
    if ((int64_t)dfd < 0) {
        return 0;
    }

    uint64_t total = 0;

    char buf[DENTS_BUF];
    for (;;) {
        int64_t nread = (int64_t)sys_getdents64(dfd, buf, sizeof(buf));
        if (nread < 0) {
            break;
        }
        if (nread == 0) {
            break;
        }

        uint64_t off = 0;
        while (off + sizeof(linux_dirent64_t) <= (uint64_t)nread) {
            linux_dirent64_t *de = (linux_dirent64_t *)(buf + off);
            if (de->d_reclen == 0) break;

            const char *name = de->d_name;
            if (!streq(name, ".") && !streq(name, "..")) {
                char child[MAX_PATH];
                if (join_path(child, sizeof(child), path, name) == 0) {
                    total += du_walk(child, depth + 1, 0, summary_only, include_files, human, printed_any);
                }
            }

            off += (uint64_t)de->d_reclen;
        }
    }

    (void)sys_close(dfd);

    if (!summary_only || is_root) {
        write_size(total, human);
        sys_puts("\t");
        sys_puts(path);
        sys_puts("\n");
        if (printed_any) *printed_any = 1;
    }
    return total;
}

static void usage(void) {
    sys_puts("usage: du [-ash] [PATH...]\n");
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    int summary_only = 0;
    int include_files = 0;
    int human = 0;

    int argi = 1;
    while (argi < argc && argv[argi] && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *opt = argv[argi] + 1;
        while (*opt) {
            if (*opt == 's') {
                summary_only = 1;
            } else if (*opt == 'a') {
                include_files = 1;
            } else if (*opt == 'h') {
                human = 1;
            } else {
                usage();
                return 1;
            }
            opt++;
        }
        argi++;
    }

    if (argi >= argc) {
        int printed_any = 0;
        (void)du_walk(".", 0, 1, summary_only, include_files, human, &printed_any);
        return printed_any ? 0 : 1;
    }

    int overall_fail = 0;
    for (int i = argi; i < argc; i++) {
        const char *p = argv[i];
        int printed_any = 0;
        (void)du_walk(p, 0, 1, summary_only, include_files, human, &printed_any);
        if (!printed_any) overall_fail = 1;
    }

    return overall_fail;

}
