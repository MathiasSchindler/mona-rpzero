#include "syscall.h"

#define AT_FDCWD ((long)-100)

enum {
    MAX_PROCS_LOCAL = 32,
    MAX_LINE = 256,
    MAX_CWD = 256,
};

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static uint64_t cstr_len_u64_local(const char *s) {
    uint64_t n = 0;
    while (s && s[n] != '\0') n++;
    return n;
}

static int streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static void puts1(const char *s) {
    (void)sys_write(1, s, cstr_len_u64_local(s));
}

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

static const char *skip_spaces(const char *p) {
    while (p && *p && is_space(*p)) p++;
    return p;
}

static const char *scan_u64(const char *p, uint64_t *out) {
    if (!p || !out) return 0;
    uint64_t v = 0;
    int any = 0;
    while (*p >= '0' && *p <= '9') {
        any = 1;
        v = v * 10u + (uint64_t)(*p - '0');
        p++;
    }
    if (!any) return 0;
    *out = v;
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

typedef struct {
    uint64_t pid;
    uint64_t ppid;
    char state;
    char cwd[MAX_CWD];
    int used;
} proc_row_t;

static int str_cmp(const char *a, const char *b) {
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    while (*a && *b) {
        if (*a != *b) return (*a < *b) ? -1 : 1;
        a++;
        b++;
    }
    if (*a == *b) return 0;
    return (*a == '\0') ? -1 : 1;
}

static void parse_ps_line(const char *line, proc_row_t *rows, int *row_count) {
    if (!line || !rows || !row_count) return;
    if (*row_count >= (int)MAX_PROCS_LOCAL) return;

    const char *p = skip_spaces(line);
    uint64_t pid = 0;
    uint64_t ppid = 0;

    p = scan_u64(p, &pid);
    if (!p) return;
    p = skip_spaces(p);
    p = scan_u64(p, &ppid);
    if (!p) return;
    p = skip_spaces(p);

    char st_s[4];
    p = scan_token(p, st_s, sizeof(st_s));
    if (!p || st_s[0] == '\0') return;
    char st = st_s[0];

    p = skip_spaces(p);

    proc_row_t *r = &rows[*row_count];
    r->pid = pid;
    r->ppid = ppid;
    r->state = st;
    r->used = 1;

    uint64_t n = 0;
    while (p && p[n] != '\0' && p[n] != '\n' && n + 1 < sizeof(r->cwd)) {
        r->cwd[n] = p[n];
        n++;
    }
    r->cwd[n] = '\0';

    (*row_count)++;
}

static int find_row_by_pid(proc_row_t *rows, int row_count, uint64_t pid) {
    for (int i = 0; i < row_count; i++) {
        if (!rows[i].used) continue;
        if (rows[i].pid == pid) return i;
    }
    return -1;
}

static void sort_indices(int *idxs, int count, proc_row_t *rows, int numeric) {
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            int a = idxs[i];
            int b = idxs[j];

            int swap = 0;
            if (numeric) {
                if (rows[a].pid > rows[b].pid) swap = 1;
            } else {
                int c = str_cmp(rows[a].cwd, rows[b].cwd);
                if (c > 0) swap = 1;
                if (c == 0 && rows[a].pid > rows[b].pid) swap = 1;
            }

            if (swap) {
                int t = idxs[i];
                idxs[i] = idxs[j];
                idxs[j] = t;
            }
        }
    }
}

static int g_show_pid = 0;
static int g_numeric = 0;
static int g_seen[MAX_PROCS_LOCAL];
static int g_stack_last[MAX_PROCS_LOCAL];

static void print_node(proc_row_t *rows, int row_count, int idx, int depth) {
    if (!rows || idx < 0 || idx >= row_count) return;
    if (g_seen[idx]) return;
    g_seen[idx] = 1;

    if (depth > 0) {
        for (int l = 0; l < depth - 1; l++) {
            puts1(g_stack_last[l] ? "   " : "|  ");
        }
        puts1(g_stack_last[depth - 1] ? "`- " : "|- ");
    }

    /* Print label */
    if (rows[idx].cwd[0] != '\0') {
        puts1(rows[idx].cwd);
    } else {
        puts1("?");
    }

    if (g_show_pid) {
        puts1("(");
        put_u64_dec(rows[idx].pid);
        puts1(")");
    }

    puts1(" [");
    putc1(rows[idx].state);
    puts1("]\n");

    /* Children */
    int kids[MAX_PROCS_LOCAL];
    int kcount = 0;
    for (int i = 0; i < row_count; i++) {
        if (!rows[i].used) continue;
        if (rows[i].ppid == rows[idx].pid && rows[i].pid != rows[i].ppid) {
            kids[kcount++] = i;
            if (kcount >= (int)MAX_PROCS_LOCAL) break;
        }
    }

    sort_indices(kids, kcount, rows, g_numeric);

    for (int i = 0; i < kcount; i++) {
        int child = kids[i];
        g_stack_last[depth] = (i == kcount - 1) ? 1 : 0;
        print_node(rows, row_count, child, depth + 1);
    }
}

static void usage(void) {
    sys_puts("usage: pstree [-p] [-n]\n");
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    for (int i = 1; i < argc; i++) {
        if (streq(argv[i], "-p")) {
            g_show_pid = 1;
            continue;
        }
        if (streq(argv[i], "-n")) {
            g_numeric = 1;
            continue;
        }
        usage();
        return 1;
    }

    long fd = (long)sys_openat((uint64_t)AT_FDCWD, "/proc/ps", 0, 0);
    if (fd < 0) {
        sys_puts("pstree: openat /proc/ps failed\n");
        return 1;
    }

    proc_row_t rows[MAX_PROCS_LOCAL];
    int row_count = 0;
    for (int i = 0; i < (int)MAX_PROCS_LOCAL; i++) {
        rows[i].used = 0;
        rows[i].pid = 0;
        rows[i].ppid = 0;
        rows[i].state = '?';
        rows[i].cwd[0] = '\0';
        g_seen[i] = 0;
        g_stack_last[i] = 1;
    }

    char acc[MAX_LINE];
    uint64_t acc_n = 0;

    char buf[128];
    for (;;) {
        long n = (long)sys_read((uint64_t)fd, buf, sizeof(buf));
        if (n < 0) {
            sys_puts("pstree: read failed\n");
            (void)sys_close((uint64_t)fd);
            return 1;
        }
        if (n == 0) break;

        for (long i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') {
                acc[acc_n] = '\0';
                parse_ps_line(acc, rows, &row_count);
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
        parse_ps_line(acc, rows, &row_count);
    }

    (void)sys_close((uint64_t)fd);

    /* Identify roots: ppid==0 or missing parent. */
    int roots[MAX_PROCS_LOCAL];
    int rcount = 0;
    for (int i = 0; i < row_count; i++) {
        if (!rows[i].used) continue;
        int parent_idx = find_row_by_pid(rows, row_count, rows[i].ppid);
        if (rows[i].ppid == 0 || parent_idx < 0) {
            roots[rcount++] = i;
            if (rcount >= (int)MAX_PROCS_LOCAL) break;
        }
    }

    sort_indices(roots, rcount, rows, g_numeric);

    for (int i = 0; i < rcount; i++) {
        int idx = roots[i];
        /* Treat each root as its own tree. */
        print_node(rows, row_count, idx, 0);
        if (i + 1 < rcount) puts1("\n");
    }

    return 0;
}
