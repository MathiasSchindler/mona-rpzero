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

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    long fd = (long)sys_openat((uint64_t)AT_FDCWD, "/", 0, 0);
    if (fd < 0) {
        sys_puts("ls: openat / failed\n");
        return 1;
    }

    char buf[512];
    for (;;) {
        long n = (long)sys_getdents64((uint64_t)fd, buf, sizeof(buf));
        if (n < 0) {
            sys_puts("ls: getdents64 failed\n");
            (void)sys_close((uint64_t)fd);
            return 1;
        }
        if (n == 0) break;

        unsigned long off = 0;
        while (off < (unsigned long)n) {
            linux_dirent64_t *d = (linux_dirent64_t *)(buf + off);
            if (d->d_reclen == 0) break;

            if (!str_eq(d->d_name, ".") && !str_eq(d->d_name, "..")) {
                sys_puts("- ");
                sys_puts(d->d_name);
                sys_puts("\n");
            }

            off += d->d_reclen;
        }
    }

    (void)sys_close((uint64_t)fd);
    return 0;
}
