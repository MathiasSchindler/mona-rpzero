#include "syscall.h"

#define AT_FDCWD ((long)-100)

int main(int argc, char **argv, char **envp) {
    (void)envp;

    const char *path = "/hello.txt";
    if (argc >= 2 && argv[1] && argv[1][0] != '\0') {
        path = argv[1];
    }

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
