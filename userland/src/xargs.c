#include "syscall.h"

static int streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static uint64_t cstr_len_u64_local(const char *s) {
    uint64_t n = 0;
    while (s && s[n] != '\0') n++;
    return n;
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int parse_u64_dec(const char *s, uint64_t *out) {
    if (!s || !out) return -1;
    if (s[0] == '\0') return -1;
    uint64_t v = 0;
    for (uint64_t i = 0; s[i] != '\0'; i++) {
        if (!is_digit(s[i])) return -1;
        v = v * 10u + (uint64_t)(s[i] - '0');
    }
    *out = v;
    return 0;
}

static void usage(void) {
    sys_puts("usage: xargs [-n N] [COMMAND [ARG...]]\n");
    sys_puts("  Minimal: whitespace-splitting; no quotes/escapes; no -0.\n");
}

static void build_exec_path(char *out, uint64_t cap, const char *cmd) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!cmd || cmd[0] == '\0') return;

    if (cmd[0] == '/') {
        uint64_t i = 0;
        while (cmd[i] && i + 1 < cap) {
            out[i] = cmd[i];
            i++;
        }
        out[i] = '\0';
        return;
    }

    const char *pre = "/bin/";
    uint64_t n = 0;
    for (uint64_t i = 0; pre[i] && n + 1 < cap; i++) out[n++] = pre[i];
    for (uint64_t i = 0; cmd[i] && n + 1 < cap; i++) out[n++] = cmd[i];
    out[n] = '\0';
}

static int run_one(const char *cmd, char **argv) {
    long pid = (long)sys_fork();
    if (pid < 0) {
        sys_puts("xargs: fork failed\n");
        return 1;
    }

    if (pid == 0) {
        char path[128];
        build_exec_path(path, sizeof(path), cmd);
        (void)sys_execve(path, (const char *const *)argv, 0);
        sys_puts("xargs: execve failed\n");
        sys_exit_group(127);
    }

    int status = 0;
    long w = (long)sys_wait4((uint64_t)pid, &status, 0, 0);
    if (w < 0) {
        sys_puts("xargs: wait4 failed\n");
        return 1;
    }

    return (status >> 8) & 0xff;
}

static void flush_batch(const char *cmd,
                        int base_argc,
                        char **base_argv,
                        char **run_argv,
                        char **tokens,
                        uint64_t *tok_count_io,
                        uint64_t *arena_pos_io,
                        int *worst_exit_io) {
    if (!tok_count_io || !arena_pos_io || !worst_exit_io) return;
    uint64_t tok_count = *tok_count_io;
    if (tok_count == 0) return;

    int ra = 0;
    for (int j = 0; j < base_argc; j++) run_argv[ra++] = base_argv[j];
    for (uint64_t j = 0; j < tok_count; j++) run_argv[ra++] = tokens[j];
    run_argv[ra] = 0;

    int ec = run_one(cmd, run_argv);
    if (ec != 0 && *worst_exit_io == 0) *worst_exit_io = ec;

    *tok_count_io = 0;
    *arena_pos_io = 0;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    uint64_t max_args_per_run = 0; /* 0 = no explicit limit (bounded by internal caps) */

    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!a) break;
        if (a[0] != '-') break;

        if (streq(a, "--")) {
            i++;
            break;
        }

        if (streq(a, "-h") || streq(a, "--help")) {
            usage();
            return 0;
        }

        if (streq(a, "-n")) {
            if (i + 1 >= argc) {
                usage();
                return 2;
            }
            uint64_t v = 0;
            if (parse_u64_dec(argv[++i], &v) != 0 || v == 0) {
                usage();
                return 2;
            }
            max_args_per_run = v;
            continue;
        }

        usage();
        return 2;
    }

    const char *cmd = 0;
    char *base_argv[32];
    int base_argc = 0;

    if (i < argc && argv[i]) {
        cmd = argv[i];
        for (; i < argc && argv[i] && base_argc + 1 < (int)(sizeof(base_argv) / sizeof(base_argv[0])); i++) {
            base_argv[base_argc++] = argv[i];
        }
    } else {
        cmd = "echo";
        base_argv[base_argc++] = "echo";
    }
    base_argv[base_argc] = 0;

    /* Combined argv per invocation. */
    char *run_argv[64];

    /* Token storage arena. */
    char arena[4096];
    uint64_t arena_pos = 0;

    /* Current batch tokens (pointers into arena). */
    char *tokens[48];
    uint64_t tok_count = 0;

    /* Token assembly buffer. */
    char tokbuf[256];
    uint64_t toklen = 0;
    int in_tok = 0;

    int saw_any_token = 0;
    int worst_exit = 0;

    /* Compute how many tokens we can pass per run. */
    uint64_t hard_cap_tokens = (uint64_t)((sizeof(tokens) / sizeof(tokens[0])));
    uint64_t hard_cap_argv = (uint64_t)((sizeof(run_argv) / sizeof(run_argv[0])));
    uint64_t avail_argv_slots = (base_argc < (int)hard_cap_argv) ? (hard_cap_argv - (uint64_t)base_argc - 1u) : 0u;

    uint64_t per_run = avail_argv_slots;
    if (max_args_per_run != 0 && max_args_per_run < per_run) per_run = max_args_per_run;
    if (per_run > hard_cap_tokens) per_run = hard_cap_tokens;

    if (per_run == 0) {
        sys_puts("xargs: too many initial args\n");
        return 1;
    }

    char inbuf[512];
    for (;;) {
        long n = (long)sys_read(0, inbuf, sizeof(inbuf));
        if (n < 0) {
            if (n == -11) continue; /* EAGAIN */
            sys_puts("xargs: read failed\n");
            return 1;
        }
        if (n == 0) break;

        for (long k = 0; k < n; k++) {
            char c = inbuf[k];
            if (is_space(c)) {
                if (in_tok) {
                    tokbuf[toklen] = '\0';

                    uint64_t need = toklen + 1;
                    if (arena_pos + need > sizeof(arena)) {
                        /* Not enough space: run current batch then retry. */
                        flush_batch(cmd, base_argc, base_argv, run_argv, tokens, &tok_count, &arena_pos, &worst_exit);
                        if (need > sizeof(arena)) {
                            sys_puts("xargs: token too long\n");
                            return 1;
                        }
                    }

                    char *p = &arena[arena_pos];
                    for (uint64_t t = 0; t < need; t++) p[t] = tokbuf[t];
                    arena_pos += need;

                    if (tok_count >= hard_cap_tokens) {
                        flush_batch(cmd, base_argc, base_argv, run_argv, tokens, &tok_count, &arena_pos, &worst_exit);
                    }

                    tokens[tok_count++] = p;
                    saw_any_token = 1;

                    if (tok_count >= per_run) {
                        flush_batch(cmd, base_argc, base_argv, run_argv, tokens, &tok_count, &arena_pos, &worst_exit);
                    }

                    toklen = 0;
                    in_tok = 0;
                }
                continue;
            }

            if (!in_tok) {
                in_tok = 1;
                toklen = 0;
            }

            if (toklen + 1 < sizeof(tokbuf)) {
                tokbuf[toklen++] = c;
            } else {
                sys_puts("xargs: token too long\n");
                return 1;
            }
        }
    }

    if (in_tok) {
        tokbuf[toklen] = '\0';
        uint64_t need = toklen + 1;
        if (arena_pos + need > sizeof(arena)) {
            flush_batch(cmd, base_argc, base_argv, run_argv, tokens, &tok_count, &arena_pos, &worst_exit);
            if (need > sizeof(arena)) {
                sys_puts("xargs: token too long\n");
                return 1;
            }
        }
        char *p = &arena[arena_pos];
        for (uint64_t t = 0; t < need; t++) p[t] = tokbuf[t];
        arena_pos += need;

        if (tok_count >= hard_cap_tokens) {
            flush_batch(cmd, base_argc, base_argv, run_argv, tokens, &tok_count, &arena_pos, &worst_exit);
        }
        tokens[tok_count++] = p;
        saw_any_token = 1;
    }

    flush_batch(cmd, base_argc, base_argv, run_argv, tokens, &tok_count, &arena_pos, &worst_exit);

    /* With no input tokens, do nothing (common safe behavior). */
    (void)cstr_len_u64_local;
    return saw_any_token ? worst_exit : 0;
}
