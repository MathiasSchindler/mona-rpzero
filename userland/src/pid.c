#include "syscall.h"

static void write_u64_dec(uint64_t v) {
    char buf[32];
    uint64_t n = 0;

    if (v == 0) {
        buf[n++] = '0';
        (void)sys_write(1, buf, n);
        return;
    }

    char tmp[32];
    uint64_t t = 0;
    while (v != 0 && t < sizeof(tmp)) {
        tmp[t++] = (char)('0' + (v % 10u));
        v /= 10u;
    }

    while (t > 0) {
        buf[n++] = tmp[--t];
    }

    (void)sys_write(1, buf, n);
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    uint64_t pid = sys_getpid();
    uint64_t ppid = sys_getppid();

    sys_puts("pid=");
    write_u64_dec(pid);
    sys_puts(" ppid=");
    write_u64_dec(ppid);
    sys_puts("\n");

    return 0;
}
