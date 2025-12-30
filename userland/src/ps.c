#include "syscall.h"

#define AT_FDCWD ((long)-100)

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static uint64_t cstr_len_u64_local(const char *s) {
    uint64_t n = 0;
    while (s && s[n] != '\0') n++;
    return n;
}

static void putc1(char c) {
    (void)sys_write(1, &c, 1);
}

static void puts1(const char *s) {
    (void)sys_write(1, s, cstr_len_u64_local(s));
}

static void put_pad_spaces(uint64_t n) {
    for (uint64_t i = 0; i < n; i++) putc1(' ');
}

static void put_col(const char *s, uint64_t width) {
    uint64_t n = cstr_len_u64_local(s);
    puts1(s);
    if (n < width) put_pad_spaces(width - n);
}

static const char *skip_spaces(const char *p) {
    while (p && *p && is_space(*p)) p++;
    return p;
}

static const char *scan_token(const char *p, char *out, uint64_t cap) {
    if (!p || !out || cap == 0) return 0;
    uint64_t n = 0;
    while (*p && !is_space(*p)) {
        if (n + 1 < cap) out[n++] = *p;
        p++;
    }
    out[n] = '\0';
    return p;
}

static void format_ps_line(const char *line) {
    char pid_s[32];
    char ppid_s[32];
    char st_s[4];

    const char *p = skip_spaces(line);
    p = scan_token(p, pid_s, sizeof(pid_s));
    p = skip_spaces(p);
    p = scan_token(p, ppid_s, sizeof(ppid_s));
    p = skip_spaces(p);
    p = scan_token(p, st_s, sizeof(st_s));
    p = skip_spaces(p);

    if (!p || pid_s[0] == '\0') return;

    put_col(pid_s, 6);
    put_col(ppid_s, 6);
    put_col(st_s, 2);
    puts1(p);
    putc1('\n');
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    long fd = (long)sys_openat((uint64_t)AT_FDCWD, "/proc/ps", 0, 0);
    if (fd < 0) {
        sys_puts("ps: openat /proc/ps failed\n");
        return 1;
    }

    puts1("PID   PPID  S CWD\n");

    /* Line-buffer the /proc/ps stream to format rows nicely. */
    char acc[256];
    uint64_t acc_n = 0;

    char buf[128];
    for (;;) {
        long n = (long)sys_read((uint64_t)fd, buf, sizeof(buf));
        if (n < 0) {
            sys_puts("ps: read failed\n");
            (void)sys_close((uint64_t)fd);
            return 1;
        }
        if (n == 0) break;

        for (long i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') {
                acc[acc_n] = '\0';
                format_ps_line(acc);
                acc_n = 0;
                continue;
            }
            if (acc_n + 1 < sizeof(acc)) {
                acc[acc_n++] = c;
            }
        }
    }

    if (acc_n != 0) {
        acc[acc_n] = '\0';
        format_ps_line(acc);
    }

    (void)sys_close((uint64_t)fd);
    return 0;
}
