#include "syscall.h"

#define AT_FDCWD ((int64_t)-100)
#define AT_REMOVEDIR 0x200u
#define EPERM 1
#define EISDIR 21
#define EROFS 30

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
    MAX_ENTRIES = 32,
    NAME_MAX_LOCAL = 64,
};

static void write_i64_dec(int64_t v) {
    char buf[32];
    uint64_t n = 0;
    if (v < 0) {
        buf[n++] = '-';
        v = -v;
    }
    if (v == 0) {
        buf[n++] = '0';
    } else {
        char tmp[32];
        uint64_t t = (uint64_t)v;
        uint64_t m = 0;
        while (t > 0 && m < sizeof(tmp)) {
            tmp[m++] = (char)('0' + (t % 10));
            t /= 10;
        }
        while (m > 0) {
            buf[n++] = tmp[--m];
        }
    }
    (void)sys_write(1, buf, n);
}

static void usage(void) {
    sys_puts("usage: rm [-f] [-r] FILE...\n");
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

static uint64_t cstr_len_u64(const char *s) {
    uint64_t n = 0;
    while (s && s[n] != '\0') n++;
    return n;
}

static int join_path(char *out, uint64_t cap, const char *base, const char *name) {
    uint64_t bl = cstr_len_u64(base);
    uint64_t nl = cstr_len_u64(name);
    if (bl == 0 || !base) return -1;
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

static int rm_path(const char *path, int recursive, int force);

static int rm_dir_children(const char *dir_path, int recursive, int force) {
    /* Deleting while iterating a directory can confuse directory offsets.
     * Robust approach: snapshot a small batch of names, close, delete them,
     * then re-open and repeat until empty.
     */
    for (int round = 0; round < 64; round++) {
        uint64_t dfd = sys_openat((uint64_t)AT_FDCWD, dir_path, 0, 0);
        if ((int64_t)dfd < 0) {
            if (!force) {
                sys_puts("rm: open dir failed rc=");
                write_i64_dec((int64_t)dfd);
                sys_puts(" path='");
                sys_puts(dir_path);
                sys_puts("'\n");
            }
            return -1;
        }

        char names[MAX_ENTRIES][NAME_MAX_LOCAL];
        int name_count = 0;

        char buf[DENTS_BUF];
        for (;;) {
            uint64_t nread = sys_getdents64(dfd, buf, sizeof(buf));
            if ((int64_t)nread < 0) {
                if (!force) {
                    sys_puts("rm: getdents64 failed rc=");
                    write_i64_dec((int64_t)nread);
                    sys_puts(" path='");
                    sys_puts(dir_path);
                    sys_puts("'\n");
                }
                (void)sys_close(dfd);
                return -1;
            }
            if (nread == 0) break;

            uint64_t pos = 0;
            while (pos + 19u <= nread) {
                linux_dirent64_t *de = (linux_dirent64_t *)(void *)(buf + pos);
                if (de->d_reclen == 0) break;
                if (pos + de->d_reclen > nread) break;

                const char *name = de->d_name;
                if (name && name[0] != '\0' && !streq(name, ".") && !streq(name, "..")) {
                    if (name_count < (int)MAX_ENTRIES) {
                        uint64_t n = cstr_len_u64(name);
                        if (n + 1 > (uint64_t)NAME_MAX_LOCAL) n = (uint64_t)NAME_MAX_LOCAL - 1;
                        for (uint64_t i = 0; i < n; i++) names[name_count][i] = name[i];
                        names[name_count][n] = '\0';
                        name_count++;
                    }
                }

                pos += de->d_reclen;
            }

            if (name_count >= (int)MAX_ENTRIES) break;
        }

        (void)sys_close(dfd);

        if (name_count == 0) {
            return 0;
        }

        for (int i = 0; i < name_count; i++) {
            char child[MAX_PATH];
            if (join_path(child, sizeof(child), dir_path, names[i]) == 0) {
                (void)rm_path(child, recursive, force);
            } else if (!force) {
                sys_puts("rm: path too long under '");
                sys_puts(dir_path);
                sys_puts("'\n");
            }
        }
    }

    if (!force) {
        sys_puts("rm: too many entries/rounds under '");
        sys_puts(dir_path);
        sys_puts("'\n");
    }
    return -1;
}

static int rm_path(const char *path, int recursive, int force) {
    uint64_t rc = sys_unlinkat((uint64_t)AT_FDCWD, path, 0);
    if ((int64_t)rc >= 0) {
        return 0;
    }

    if (recursive && ((int64_t)rc == -(int64_t)EISDIR || (int64_t)rc == -(int64_t)EPERM || (int64_t)rc == -(int64_t)EROFS)) {
        /* Some kernels return EPERM/EROFS when attempting to unlink a directory.
         * Try treating it as a directory if we can open it.
         */
        if (rm_dir_children(path, recursive, force) == 0) {
            uint64_t drc = sys_unlinkat((uint64_t)AT_FDCWD, path, (uint64_t)AT_REMOVEDIR);
            if ((int64_t)drc >= 0) return 0;
            rc = drc;
        }
    }

    if (!force) {
        sys_puts("rm: unlinkat failed rc=");
        write_i64_dec((int64_t)rc);
        sys_puts(" path='");
        sys_puts(path);
        sys_puts("'\n");
    }
    return -1;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    int force = 0;
    int recursive = 0;
    int i = 1;
    if (i < argc && argv[i] && argv[i][0] == '-' && argv[i][1] != '\0') {
        while (i < argc && argv[i] && argv[i][0] == '-' && argv[i][1] != '\0') {
            if (argv[i][0] == '-' && argv[i][1] == 'f' && argv[i][2] == '\0') {
                force = 1;
                i++;
                continue;
            }
            if (argv[i][0] == '-' && argv[i][1] == 'r' && argv[i][2] == '\0') {
                recursive = 1;
                i++;
                continue;
            }
            usage();
            return 1;
        }
    }

    if (i >= argc) {
        usage();
        return 1;
    }

    int rc_any = 0;
    for (; i < argc; i++) {
        const char *path = argv[i];
        if (!path || path[0] == '\0') continue;

        if (rm_path(path, recursive, force) != 0) {
            rc_any = 1;
        }
    }

    return rc_any;
}
