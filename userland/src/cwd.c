#include "syscall.h"

static void write_u64_dec(uint64_t v) {
    char buf[32];
    uint64_t n = 0;
    if (v == 0) {
        buf[n++] = '0';
    } else {
        char tmp[32];
        uint64_t t = v;
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

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    char buf[128];
    uint64_t rc = sys_getcwd(buf, sizeof(buf));
    sys_puts("getcwd rc=");
    write_u64_dec(rc);
    sys_puts(" cwd=");
    sys_puts(buf);
    sys_puts("\n");

    rc = sys_chdir("/bin");
    sys_puts("chdir /bin rc=");
    write_u64_dec(rc);
    sys_puts("\n");

    rc = sys_getcwd(buf, sizeof(buf));
    sys_puts("cwd=");
    sys_puts(buf);
    sys_puts("\n");

    /* Validate relative open works after chdir. */
    uint64_t fd = sys_openat((uint64_t)(int64_t)-100, "sh", 0, 0);
    sys_puts("openat 'sh' fd=");
    write_u64_dec(fd);
    sys_puts("\n");
    if ((int64_t)fd >= 0) (void)sys_close(fd);

    sys_puts("cwd: OK\n");
    return 0;
}
