#include "syscall.h"

#define AT_FDCWD ((long)-100)

int main(int argc, char **argv, char **envp) {
    (void)envp;

    /* If no path argument is given, behave like a basic `cat` and copy stdin to stdout. */
    if (argc < 2 || !argv[1] || argv[1][0] == '\0') {
        char buf[256];
        for (;;) {
            long n = (long)sys_read(0, buf, sizeof(buf));
            if (n == 0) break;
            if (n < 0) {
                /* EAGAIN (11) => retry (used for pipes). */
                if (n == -11) {
                    continue;
                }
                sys_puts("cat: read failed\n");
                return 1;
            }
            (void)sys_write(1, buf, (uint64_t)n);
        }
        return 0;
    }

    const char *path = argv[1];

    long fd = (long)sys_openat((uint64_t)AT_FDCWD, path, 0, 0);
    if (fd < 0) {
        sys_puts("cat: openat failed\n");
        return 1;
    }

    char buf[256];
    for (;;) {
        long n = (long)sys_read((uint64_t)fd, buf, sizeof(buf));
        if (n < 0) {
            sys_puts("cat: read failed\n");
            (void)sys_close((uint64_t)fd);
            return 1;
        }
        if (n == 0) break;
        (void)sys_write(1, buf, (uint64_t)n);
    }

    (void)sys_close((uint64_t)fd);
    return 0;
}
