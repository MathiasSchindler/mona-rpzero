#include "syscall.h"

#define AT_FDCWD ((long)-100)

typedef struct {
    unsigned long long d_ino;
    long long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
} linux_dirent64_t;

static int str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int str_starts_with(const char *s, char c) {
    return s && s[0] == c;
}

static const char *path_basename(const char *path) {
    if (!path) return "";

    /* Skip trailing slashes. */
    uint64_t n = 0;
    while (path[n] != '\0') n++;
    while (n > 0 && path[n - 1] == '/') n--;
    if (n == 0) return "/";

    uint64_t last = 0;
    for (uint64_t i = 0; i < n; i++) {
        if (path[i] == '/') last = i + 1;
    }
    return &path[last];
}

static void usage(void) {
    sys_puts("usage: ls [-a] [-l] [PATH...]\n");
}

static uint64_t cstr_len_u64_local(const char *s) {
    uint64_t n = 0;
    while (s && s[n] != '\0') n++;
    return n;
}

static void u64_to_dec(char *out, uint64_t cap, uint64_t v) {
    if (cap == 0) return;
    char tmp[24];
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

static void mode_to_perm(char out[11], uint32_t mode) {
    out[0] = ((mode & 0170000u) == 0040000u) ? 'd' : '-';
    const char rwx[] = {'r', 'w', 'x'};
    for (int i = 0; i < 9; i++) {
        uint32_t bit = 1u << (8 - (uint32_t)i);
        out[1 + i] = (mode & bit) ? rwx[i % 3] : '-';
    }
    out[10] = '\0';
}

static int join_path(char *out, uint64_t cap, const char *dir, const char *name) {
    if (!out || cap == 0 || !dir || !name) return -1;
    uint64_t dlen = cstr_len_u64_local(dir);
    uint64_t nlen = cstr_len_u64_local(name);
    if (dlen == 0) return -1;

    /* If dir is "/", avoid double slashes. */
    int dir_is_root = (dlen == 1 && dir[0] == '/');
    uint64_t need = dlen + (dir_is_root ? 0u : 1u) + nlen + 1u;
    if (need > cap) return -1;

    uint64_t o = 0;
    for (uint64_t i = 0; i < dlen; i++) out[o++] = dir[i];
    if (!dir_is_root) out[o++] = '/';
    for (uint64_t i = 0; i < nlen; i++) out[o++] = name[i];
    out[o] = '\0';
    return 0;
}

static void print_long(const char *name, const linux_stat_t *st) {
    char perm[11];
    mode_to_perm(perm, st->st_mode);
    sys_puts(perm);
    sys_puts(" ");

    char sz[24];
    uint64_t size = (st->st_size < 0) ? 0u : (uint64_t)st->st_size;
    u64_to_dec(sz, sizeof(sz), size);
    sys_puts(sz);
    sys_puts(" ");
    sys_puts(name);
    sys_puts("\n");
}

static int list_dir(const char *path, int opt_all, int opt_long, int show_header) {
    long fd = (long)sys_openat((uint64_t)AT_FDCWD, path, 0, 0);
    if (fd < 0) {
        sys_puts("ls: cannot open: ");
        sys_puts(path);
        sys_puts("\n");
        return 1;
    }

    if (show_header) {
        sys_puts(path);
        sys_puts(":\n");
    }

    if (opt_all) {
        if (opt_long) {
            linux_stat_t st;
            char full[512];

            if (join_path(full, sizeof(full), path, ".") == 0 && (long)sys_newfstatat((uint64_t)AT_FDCWD, full, &st, 0) >= 0) {
                print_long(".", &st);
            } else {
                sys_puts("?--------- 0 .\n");
            }

            if (join_path(full, sizeof(full), path, "..") == 0 && (long)sys_newfstatat((uint64_t)AT_FDCWD, full, &st, 0) >= 0) {
                print_long("..", &st);
            } else {
                sys_puts("?--------- 0 ..\n");
            }
        } else {
            sys_puts(".\n");
            sys_puts("..\n");
        }
    }

    char buf[512];
    int status = 0;
    for (;;) {
        long n = (long)sys_getdents64((uint64_t)fd, buf, sizeof(buf));
        if (n < 0) {
            sys_puts("ls: getdents64 failed\n");
            status = 1;
            break;
        }
        if (n == 0) break;

        unsigned long off = 0;
        while (off < (unsigned long)n) {
            linux_dirent64_t *d = (linux_dirent64_t *)(buf + off);
            if (d->d_reclen == 0) break;

            const char *name = d->d_name;
            if (opt_all) {
                /* We already emitted synthetic '.' and '..' above. */
                if (str_eq(name, ".") || str_eq(name, "..")) {
                    off += d->d_reclen;
                    continue;
                }
            } else {
                if (str_starts_with(name, '.')) {
                    off += d->d_reclen;
                    continue;
                }
            }

            if (opt_long) {
                linux_stat_t st;
                char full[512];
                if (join_path(full, sizeof(full), path, name) == 0 && (long)sys_newfstatat((uint64_t)AT_FDCWD, full, &st, 0) >= 0) {
                    print_long(name, &st);
                } else {
                    sys_puts("?--------- 0 ");
                    sys_puts(name);
                    sys_puts("\n");
                }
            } else {
                sys_puts(name);
                sys_puts("\n");
            }

            off += d->d_reclen;
        }
    }

    (void)sys_close((uint64_t)fd);
    return status;
}

static int list_path(const char *path, int opt_all, int opt_long, int show_header) {
    linux_stat_t st;
    long rc = (long)sys_newfstatat((uint64_t)AT_FDCWD, path, &st, 0);
    if (rc < 0) {
        sys_puts("ls: cannot stat: ");
        sys_puts(path);
        sys_puts("\n");
        return 1;
    }

    if ((st.st_mode & 0170000u) == 0040000u) {
        return list_dir(path, opt_all, opt_long, show_header);
    }

    if (show_header) {
        sys_puts(path);
        sys_puts(":\n");
    }

    (void)opt_all;
    if (opt_long) {
        print_long(path_basename(path), &st);
    } else {
        sys_puts(path_basename(path));
        sys_puts("\n");
    }
    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    int opt_all = 0;
    int opt_long = 0;

    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!a || a[0] == '\0') break;
        if (a[0] != '-') break;
        if (str_eq(a, "--")) {
            i++;
            break;
        }

        if (str_eq(a, "-a")) {
            opt_all = 1;
            continue;
        }

        if (str_eq(a, "-l")) {
            opt_long = 1;
            continue;
        }

        /* Combined short opts: -al, -la */
        if (a[1] != '\0' && a[2] != '\0' && a[1] != '-') {
            int ok = 1;
            for (int j = 1; a[j] != '\0'; j++) {
                if (a[j] == 'a') opt_all = 1;
                else if (a[j] == 'l') opt_long = 1;
                else {
                    ok = 0;
                    break;
                }
            }
            if (ok) continue;
        }

        sys_puts("ls: unknown option\n");
        usage();
        return 2;
    }

    int npaths = argc - i;
    if (npaths <= 0) {
        return list_path(".", opt_all, opt_long, 0);
    }

    int status = 0;
    for (int p = 0; p < npaths; p++) {
        const char *path = argv[i + p];
        if (!path || path[0] == '\0') continue;

        int show_header = (npaths > 1);
        if (p > 0) sys_puts("\n");
        status |= list_path(path, opt_all, opt_long, show_header);
    }

    return status ? 1 : 0;
}
