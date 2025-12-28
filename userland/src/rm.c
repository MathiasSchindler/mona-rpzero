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
    sys_puts("usage: rm [-f] FILE...\n");
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    int force = 0;
    int i = 1;
    if (i < argc && argv[i] && argv[i][0] == '-' && argv[i][1] != '\0') {
        if (argv[i][0] == '-' && argv[i][1] == 'f' && argv[i][2] == '\0') {
            force = 1;
            i++;
        } else {
            usage();
            return 1;
        }
    }

    if (i >= argc) {
        usage();
        return 1;
    }

    int rc_any = 0;
    for (; i < argc; i++) {
        const char *path = argv[i];
        if (!path || path[0] == '\0') continue;

        uint64_t rc = sys_unlinkat((uint64_t)AT_FDCWD, path, 0);
        if ((int64_t)rc < 0) {
            if (!force) {
                sys_puts("rm: unlinkat failed rc=");
                write_i64_dec((int64_t)rc);
                sys_puts(" path='");
                sys_puts(path);
                sys_puts("'\n");
                rc_any = 1;
            }
        }
    }

    return rc_any;
}
