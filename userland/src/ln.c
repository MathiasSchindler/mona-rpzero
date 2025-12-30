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
    sys_puts("usage: ln [-s] TARGET LINK_NAME\n");
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    int symlink_mode = 0;
    int argi = 1;
    if (argc >= 2 && argv[1] && argv[1][0] == '-' && argv[1][1] != '\0') {
        if (argv[1][1] == 's' && argv[1][2] == '\0') {
            symlink_mode = 1;
            argi = 2;
        } else {
            usage();
            return 1;
        }
    }

    if (argc - argi != 2) {
        usage();
        return 1;
    }

    const char *target = argv[argi];
    const char *link_name = argv[argi + 1];

    int64_t rc;
    if (symlink_mode) {
        rc = (int64_t)sys_symlinkat(target, AT_FDCWD, link_name);
    } else {
        rc = (int64_t)sys_linkat(AT_FDCWD, target, AT_FDCWD, link_name, 0);
    }
    if (rc < 0) {
        sys_puts("ln: failed rc=");
        write_i64_dec(rc);
        sys_puts("\n");
        return 1;
    }

    return 0;
}
