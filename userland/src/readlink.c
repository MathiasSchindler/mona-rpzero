#include "syscall.h"

#define AT_FDCWD ((int64_t)-100)

enum {
    MAX_PATH = 256,
};

static uint64_t cstr_len_u64_local(const char *s) {
    uint64_t n = 0;
    while (s && s[n] != '\0') n++;
    return n;
}

static void usage(void) {
    sys_puts("usage: readlink PATH\n");
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    if (argc != 2) {
        usage();
        return 1;
    }

    char buf[MAX_PATH];
    int64_t n = (int64_t)sys_readlinkat(AT_FDCWD, argv[1], buf, sizeof(buf));
    if (n < 0) {
        sys_puts("readlink: failed\n");
        return 1;
    }

    /* readlinkat does not NUL-terminate. */
    if ((uint64_t)n > sizeof(buf)) n = (int64_t)sizeof(buf);
    (void)sys_write(1, buf, (uint64_t)n);
    sys_puts("\n");

    (void)cstr_len_u64_local;
    return 0;
}
