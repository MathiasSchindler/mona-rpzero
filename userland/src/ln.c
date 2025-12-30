#include "syscall.h"

#define AT_FDCWD ((int64_t)-100)

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
    sys_puts("usage: ln TARGET LINK_NAME\n");
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    if (argc != 3) {
        usage();
        return 1;
    }

    const char *target = argv[1];
    const char *link_name = argv[2];

    int64_t rc = (int64_t)sys_linkat(AT_FDCWD, target, AT_FDCWD, link_name, 0);
    if (rc < 0) {
        sys_puts("ln: linkat failed rc=");
        write_i64_dec(rc);
        sys_puts("\n");
        return 1;
    }

    return 0;
}
