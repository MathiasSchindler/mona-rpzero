#include "syscall.h"

#define AT_FDCWD ((int64_t)-100)

/* openat(2) flags subset (match kernel sys_fs.c). */
#define O_RDONLY 0u
#define O_WRONLY 1u
#define O_CREAT 0100u
#define O_TRUNC 01000u

/* st_mode file types (POSIX). */
#define S_IFMT 0170000u
#define S_IFDIR 0040000u
#define S_IFREG 0100000u

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

static void write_i64_dec_local(int64_t v) {
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
            tmp[m++] = (char)('0' + (t % 10u));
            t /= 10u;
        }
        while (m > 0) buf[n++] = tmp[--m];
    }
    (void)sys_write(1, buf, n);
}

static int write_all(uint64_t fd, const char *buf, uint64_t len) {
    uint64_t off = 0;
    while (off < len) {
        long rc = (long)sys_write(fd, buf + off, len - off);
        if (rc < 0) {
            /* EAGAIN (11) => retry (pipes). */
            if (rc == -11) continue;
            return -1;
        }
        if (rc == 0) return -1;
        off += (uint64_t)rc;
    }
    return 0;
}

static const char *basename_ptr(const char *path) {
    if (!path) return "";

    /* Skip trailing slashes. */
    uint64_t n = cstr_len_u64_local(path);
    while (n > 0 && path[n - 1] == '/') n--;
    if (n == 0) return "/";

    uint64_t last = 0;
    for (uint64_t i = 0; i < n; i++) {
        if (path[i] == '/') last = i + 1;
    }
    return path + last;
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

static int mode_is_dir(uint32_t mode) {
    return (mode & S_IFMT) == S_IFDIR;
}

static int mode_is_reg(uint32_t mode) {
    return (mode & S_IFMT) == S_IFREG;
}

static void usage(void) {
    sys_puts("usage: mv SOURCE... DEST\n");
    sys_puts("       mv SOURCE DEST\n");
    sys_puts("       mv -h|--help\n");
}

static int copy_file(const char *src, const char *dst, uint32_t create_mode) {
    int64_t in_opened = (int64_t)sys_openat((uint64_t)AT_FDCWD, src, (uint64_t)O_RDONLY, 0);
    if (in_opened < 0) {
        sys_puts("mv: cannot open: ");
        sys_puts(src);
        sys_puts(" rc=");
        write_i64_dec_local(in_opened);
        sys_puts("\n");
        return -1;
    }
    uint64_t in_fd = (uint64_t)in_opened;

    uint64_t flags = (uint64_t)(O_WRONLY | O_CREAT | O_TRUNC);
    int64_t out_opened = (int64_t)sys_openat((uint64_t)AT_FDCWD, dst, flags, create_mode);
    if (out_opened < 0) {
        sys_puts("mv: cannot open dest: ");
        sys_puts(dst);
        sys_puts(" rc=");
        write_i64_dec_local(out_opened);
        sys_puts("\n");
        (void)sys_close(in_fd);
        return -1;
    }
    uint64_t out_fd = (uint64_t)out_opened;

    char buf[4096];
    for (;;) {
        long n = (long)sys_read(in_fd, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) {
            if (n == -11) continue;
            sys_puts("mv: read failed rc=");
            write_i64_dec_local((int64_t)n);
            sys_puts("\n");
            (void)sys_close(out_fd);
            (void)sys_close(in_fd);
            return -1;
        }
        if (write_all(out_fd, buf, (uint64_t)n) != 0) {
            sys_puts("mv: write failed\n");
            (void)sys_close(out_fd);
            (void)sys_close(in_fd);
            return -1;
        }
    }

    (void)sys_close(out_fd);
    (void)sys_close(in_fd);
    return 0;
}

static int mv_one(const char *src, const char *dst) {
    if (!src || !dst || src[0] == '\0' || dst[0] == '\0') return -1;
    if (streq(src, "-")) {
        sys_puts("mv: stdin source ('-') not supported\n");
        return -1;
    }

    linux_stat_t st;
    int64_t src_stat_rc = (int64_t)sys_newfstatat((uint64_t)AT_FDCWD, src, &st, 0);
    if (src_stat_rc < 0) {
        sys_puts("mv: cannot stat: ");
        sys_puts(src);
        sys_puts(" rc=");
        write_i64_dec_local(src_stat_rc);
        sys_puts("\n");
        return -1;
    }
    if (mode_is_dir(st.st_mode)) {
        sys_puts("mv: cannot move directory: ");
        sys_puts(src);
        sys_puts("\n");
        return -1;
    }
    if (!mode_is_reg(st.st_mode)) {
        sys_puts("mv: unsupported file type: ");
        sys_puts(src);
        sys_puts("\n");
        return -1;
    }

    uint32_t create_mode = (uint32_t)(st.st_mode & 0777u);
    if (copy_file(src, dst, create_mode) != 0) {
        return -1;
    }

    int64_t urc = (int64_t)sys_unlinkat((uint64_t)AT_FDCWD, src, 0);
    if (urc < 0) {
        sys_puts("mv: unlink failed: ");
        sys_puts(src);
        sys_puts(" rc=");
        write_i64_dec_local(urc);
        sys_puts("\n");
        return -1;
    }

    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    if (argc >= 2 && argv[1] && (streq(argv[1], "-h") || streq(argv[1], "--help"))) {
        usage();
        return 0;
    }

    if (argc < 3) {
        usage();
        return 1;
    }

    int nsrc = argc - 2;
    const char *dst = argv[argc - 1];

    int dst_is_dir = 0;
    if (nsrc > 1) {
        linux_stat_t st;
        int64_t rc = (int64_t)sys_newfstatat((uint64_t)AT_FDCWD, dst, &st, 0);
        if (rc < 0 || !mode_is_dir(st.st_mode)) {
            sys_puts("mv: destination is not a directory\n");
            return 1;
        }
        dst_is_dir = 1;
    } else {
        linux_stat_t st;
        int64_t rc = (int64_t)sys_newfstatat((uint64_t)AT_FDCWD, dst, &st, 0);
        if (rc >= 0 && mode_is_dir(st.st_mode)) {
            dst_is_dir = 1;
        }
    }

    int status = 0;
    for (int i = 1; i < 1 + nsrc; i++) {
        const char *src = argv[i];
        if (!src || src[0] == '\0') continue;

        char dst_path[256];
        const char *dst_use = dst;
        if (dst_is_dir) {
            const char *bn = basename_ptr(src);
            if (join_path(dst_path, sizeof(dst_path), dst, bn) != 0) {
                sys_puts("mv: destination path too long\n");
                status = 1;
                continue;
            }
            dst_use = dst_path;
        }

        if (mv_one(src, dst_use) != 0) {
            status = 1;
        }
    }

    return status;
}
