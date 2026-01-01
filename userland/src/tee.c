#include "syscall.h"

#define AT_FDCWD ((long)-100)

/* openat(2) flags subset (match kernel sys_fs.c). */
#define O_WRONLY 1u
#define O_CREAT 0100u
#define O_TRUNC 01000u

static int streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
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

static void usage(void) {
    sys_puts("usage: tee [FILE...]\n");
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    if (argc >= 2 && argv[1] && (streq(argv[1], "-h") || streq(argv[1], "--help"))) {
        usage();
        return 0;
    }

    /* Open output files. */
    enum { MAX_OUT = 16 };
    uint64_t fds[MAX_OUT];
    int nfds = 0;

    for (int i = 1; i < argc; i++) {
        const char *path = argv[i];
        if (!path || path[0] == '\0') continue;

        if (nfds >= MAX_OUT) {
            sys_puts("tee: too many files\n");
            break;
        }

        uint64_t flags = (uint64_t)(O_WRONLY | O_CREAT | O_TRUNC);
        long fd = (long)sys_openat((uint64_t)AT_FDCWD, path, flags, 0644);
        if (fd < 0) {
            sys_puts("tee: openat failed\n");
            /* still continue, mirroring stdout */
            continue;
        }
        fds[nfds++] = (uint64_t)fd;
    }

    char buf[512];
    for (;;) {
        long n = (long)sys_read(0, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) {
            /* EAGAIN (11) => retry (pipes). */
            if (n == -11) continue;
            sys_puts("tee: read failed\n");
            for (int i = 0; i < nfds; i++) (void)sys_close(fds[i]);
            return 1;
        }

        if (write_all(1, buf, (uint64_t)n) != 0) {
            sys_puts("tee: write failed\n");
            for (int i = 0; i < nfds; i++) (void)sys_close(fds[i]);
            return 1;
        }

        for (int i = 0; i < nfds; i++) {
            (void)write_all(fds[i], buf, (uint64_t)n);
        }
    }

    for (int i = 0; i < nfds; i++) (void)sys_close(fds[i]);
    return 0;
}
