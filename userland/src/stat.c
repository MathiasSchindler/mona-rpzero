#include "syscall.h"

#define AT_FDCWD ((int64_t)-100)

/* st_mode file types (POSIX). */
#define S_IFMT 0170000u
#define S_IFDIR 0040000u
#define S_IFREG 0100000u
#define S_IFLNK 0120000u

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

    char out[32];
    uint64_t o = 0;
    while (n > 0 && o + 1 < sizeof(out)) {
        out[o++] = tmp[--n];
    }
    (void)sys_write(1, out, o);
}

static void write_u32_octal_mode(uint32_t mode) {
    /* Print as 4 octal digits like 0755 (lowest 12 bits). */
    char buf[4];
    uint32_t v = mode & 07777u;
    buf[0] = (char)('0' + (char)((v >> 9) & 7u));
    buf[1] = (char)('0' + (char)((v >> 6) & 7u));
    buf[2] = (char)('0' + (char)((v >> 3) & 7u));
    buf[3] = (char)('0' + (char)(v & 7u));
    (void)sys_write(1, buf, sizeof(buf));
}

static const char *mode_type(uint32_t mode) {
    uint32_t t = mode & S_IFMT;
    if (t == S_IFDIR) return "directory";
    if (t == S_IFREG) return "regular file";
    if (t == S_IFLNK) return "symlink";
    return "unknown";
}

static void usage(void) {
    sys_puts("usage: stat FILE...\n");
    sys_puts("       stat -h|--help\n");
}

static int stat_one(const char *path) {
    linux_stat_t st;
    int64_t rc = (int64_t)sys_newfstatat((uint64_t)AT_FDCWD, path, &st, 0);
    if (rc < 0) {
        sys_puts("stat: cannot stat: ");
        sys_puts(path);
        sys_puts("\n");
        return -1;
    }

    sys_puts("  File: ");
    sys_puts(path);
    sys_puts("\n");

    sys_puts("  Type: ");
    sys_puts(mode_type(st.st_mode));
    sys_puts("\n");

    sys_puts("  Mode: ");
    write_u32_octal_mode(st.st_mode);
    sys_puts("\n");

    sys_puts("  Links: ");
    write_u64_dec(st.st_nlink);
    sys_puts("\n");

    sys_puts("  Size: ");
    if (st.st_size < 0) {
        sys_puts("0");
    } else {
        write_u64_dec((uint64_t)st.st_size);
    }
    sys_puts("\n");

    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    if (argc >= 2 && argv[1] && (streq(argv[1], "-h") || streq(argv[1], "--help"))) {
        usage();
        return 0;
    }

    if (argc < 2) {
        usage();
        return 1;
    }

    int status = 0;
    for (int i = 1; i < argc; i++) {
        const char *p = argv[i];
        if (!p || p[0] == '\0') continue;
        if (stat_one(p) != 0) status = 1;
    }

    return status;
}
