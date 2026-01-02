#include "syscall.h"

#define AT_FDCWD ((int64_t)-100)

enum {
    O_RDONLY = 0,
};

static void putc1(char c) {
    (void)sys_write(1, &c, 1);
}

static void put_u64_dec(uint64_t v) {
    char tmp[32];
    uint64_t t = 0;

    if (v == 0) {
        putc1('0');
        return;
    }
    while (v != 0 && t < sizeof(tmp)) {
        tmp[t++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (t > 0) {
        putc1(tmp[--t]);
    }
}

static void put_spaces(uint64_t n) {
    for (uint64_t i = 0; i < n; i++) {
        putc1(' ');
    }
}

static uint64_t cstr_len(const char *s) {
    uint64_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}

static int starts_with(const char *s, const char *prefix) {
    uint64_t i = 0;
    while (prefix[i] != '\0') {
        if (s[i] != prefix[i]) return 0;
        i++;
    }
    return 1;
}

static int parse_u64(const char *s, uint64_t *out) {
    uint64_t v = 0;
    uint64_t i = 0;

    while (s[i] == ' ' || s[i] == '\t') i++;

    int any = 0;
    while (s[i] >= '0' && s[i] <= '9') {
        any = 1;
        v = v * 10u + (uint64_t)(s[i] - '0');
        i++;
    }

    if (!any) return -1;
    *out = v;
    return 0;
}

static void print_row(const char *label, uint64_t total_kb, uint64_t used_kb, uint64_t free_kb) {
    (void)sys_write(1, label, cstr_len(label));

    /* keep it simple: fixed-ish spacing (not perfectly aligned for huge numbers) */
    put_spaces(3);
    put_u64_dec(total_kb);
    put_spaces(3);
    put_u64_dec(used_kb);
    put_spaces(3);
    put_u64_dec(free_kb);
    putc1('\n');
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    int64_t fd = (int64_t)sys_openat((uint64_t)AT_FDCWD, "/proc/meminfo", (uint64_t)O_RDONLY, 0);
    if (fd < 0) {
        sys_puts("free: open /proc/meminfo failed\n");
        return 1;
    }

    /* Read entire file into a small buffer (procfs output is tiny). */
    char buf[512];
    int64_t n = (int64_t)sys_read((uint64_t)fd, buf, sizeof(buf) - 1);
    (void)sys_close((uint64_t)fd);

    if (n < 0) {
        sys_puts("free: read failed\n");
        return 1;
    }
    buf[n] = '\0';

    uint64_t mem_total_kb = 0;
    uint64_t mem_free_kb = 0;

    /* Parse a couple of lines we provide in the kernel's /proc/meminfo. */
    const char *p = buf;
    while (*p != '\0') {
        const char *line = p;
        while (*p != '\0' && *p != '\n') p++;
        uint64_t line_len = (uint64_t)(p - line);
        if (*p == '\n') p++;

        /* Temporarily NUL-terminate the line for parsing. */
        char tmp;
        if (line_len < sizeof(buf)) {
            tmp = line[line_len];
            ((char *)line)[line_len] = '\0';
        } else {
            tmp = 0;
        }

        if (starts_with(line, "MemTotal:")) {
            (void)parse_u64(line + 9, &mem_total_kb);
        } else if (starts_with(line, "MemFree:")) {
            (void)parse_u64(line + 8, &mem_free_kb);
        }

        if (line_len < sizeof(buf)) {
            ((char *)line)[line_len] = tmp;
        }
    }

    if (mem_total_kb == 0) {
        sys_puts("free: no MemTotal in /proc/meminfo\n");
        return 1;
    }

    uint64_t used_kb = (mem_total_kb >= mem_free_kb) ? (mem_total_kb - mem_free_kb) : 0;

    sys_puts("              total   used   free\n");
    print_row("Mem:", mem_total_kb, used_kb, mem_free_kb);
    return 0;
}
