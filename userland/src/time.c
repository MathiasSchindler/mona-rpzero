#include "syscall.h"

/* Minimal time(1): wall-clock (monotonic) around a child process.
 * Usage: time COMMAND [ARG...]
 */

static uint64_t cstr_len_u64_local(const char *s) {
    uint64_t n = 0;
    while (s && s[n] != '\0') n++;
    return n;
}

static void write_u64_dec(uint64_t v) {
    char tmp[32];
    uint64_t t = 0;
    if (v == 0) {
        tmp[t++] = '0';
    } else {
        char rev[32];
        uint64_t r = 0;
        while (v > 0 && r < sizeof(rev)) {
            rev[r++] = (char)('0' + (v % 10u));
            v /= 10u;
        }
        while (r > 0) tmp[t++] = rev[--r];
    }
    (void)sys_write(1, tmp, t);
}

static void write_ns_as_seconds(uint64_t ns) {
    uint64_t sec = ns / 1000000000ull;
    uint64_t nsec = ns % 1000000000ull;
    write_u64_dec(sec);
    (void)sys_write(1, ".", 1);

    /* Print 3 decimals (ms). */
    uint64_t ms = nsec / 1000000ull;
    char buf[3];
    buf[0] = (char)('0' + (char)((ms / 100u) % 10u));
    buf[1] = (char)('0' + (char)((ms / 10u) % 10u));
    buf[2] = (char)('0' + (char)(ms % 10u));
    (void)sys_write(1, buf, 3);
}

static void usage(void) {
    sys_puts("usage: time COMMAND [ARG...]\n");
}

static int contains_slash(const char *s) {
    if (!s) return 0;
    for (uint64_t i = 0; s[i] != '\0'; i++) {
        if (s[i] == '/') return 1;
    }
    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    if (argc < 2) {
        usage();
        return 1;
    }

    linux_timespec_t t0;
    linux_timespec_t t1;
    (void)sys_clock_gettime(1, &t0);

    int64_t pid = (int64_t)sys_fork();
    if (pid == 0) {
        /* execve(2) does not search PATH; match our shell behavior (/bin/<cmd>). */
        const char *cmd = argv[1];
        char path[128];
        if (!cmd) {
            sys_puts("time: execve failed\n");
            sys_exit_group(127);
        }

        if (cmd[0] == '/' || contains_slash(cmd)) {
            /* Absolute or explicit relative path (./foo, dir/foo, etc.) */
            uint64_t i = 0;
            while (cmd[i] && i + 1 < (uint64_t)sizeof(path)) {
                path[i] = cmd[i];
                i++;
            }
            path[i] = '\0';
        } else {
            /* /bin/<cmd> */
            const char *pre = "/bin/";
            uint64_t i = 0;
            while (pre[i] && i + 1 < (uint64_t)sizeof(path)) {
                path[i] = pre[i];
                i++;
            }
            uint64_t j = 0;
            while (cmd[j] && i + 1 < (uint64_t)sizeof(path)) {
                path[i++] = cmd[j++];
            }
            path[i] = '\0';
        }

        (void)sys_execve(path, (const char *const *)&argv[1], 0);
        sys_puts("time: execve failed\n");
        sys_exit_group(127);
    }
    if (pid < 0) {
        sys_puts("time: fork failed\n");
        return 1;
    }

    int status = 0;
    if ((int64_t)sys_wait4(pid, &status, 0, 0) < 0) {
        sys_puts("time: wait4 failed\n");
        return 1;
    }

    (void)sys_clock_gettime(1, &t1);

    uint64_t ns0 = (uint64_t)t0.tv_sec * 1000000000ull + (uint64_t)t0.tv_nsec;
    uint64_t ns1 = (uint64_t)t1.tv_sec * 1000000000ull + (uint64_t)t1.tv_nsec;
    uint64_t dns = (ns1 >= ns0) ? (ns1 - ns0) : 0;

    sys_puts("real\t");
    write_ns_as_seconds(dns);
    sys_puts("\n");

    (void)cstr_len_u64_local;

    int exit_code = (status >> 8) & 0xff;
    return exit_code;
}
