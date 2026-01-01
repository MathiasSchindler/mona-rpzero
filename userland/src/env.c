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

static void usage(void) {
    sys_puts("usage: env [-i] [NAME=VALUE ...] [COMMAND [ARG...]]\n");
}

static int has_equal(const char *s) {
    if (!s) return 0;
    for (uint64_t i = 0; s[i] != '\0'; i++) {
        if (s[i] == '=') return 1;
    }
    return 0;
}

int main(int argc, char **argv, char **envp) {
    int opt_i = 0;

    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!a || a[0] != '-') break;
        if (streq(a, "--")) {
            i++;
            break;
        }
        if (streq(a, "-h") || streq(a, "--help")) {
            usage();
            return 0;
        }
        if (streq(a, "-i")) {
            opt_i = 1;
            continue;
        }
        /* unknown option */
        usage();
        return 2;
    }

    /* Collect NAME=VALUE assignments. */
    enum { MAX_ENV = 64 };
    const char *assign[MAX_ENV];
    int nassign = 0;

    while (i < argc && argv[i] && argv[i][0] != '\0' && has_equal(argv[i])) {
        if (nassign < (int)MAX_ENV) {
            assign[nassign++] = argv[i];
        }
        i++;
    }

    /* If a command follows, exec it with the environment we constructed. */
    if (i < argc && argv[i]) {
        const char *cmd = argv[i];
        const char *const *child_argv = (const char *const *)&argv[i];

        const char *newenv[MAX_ENV];
        int nenv = 0;

        if (!opt_i && envp) {
            for (int j = 0; envp[j]; j++) {
                if (nenv + 1 >= (int)MAX_ENV) break;
                newenv[nenv++] = envp[j];
            }
        }

        for (int j = 0; j < nassign; j++) {
            if (nenv + 1 >= (int)MAX_ENV) break;
            newenv[nenv++] = assign[j];
        }
        newenv[nenv] = 0;

        (void)sys_execve(cmd, child_argv, (const char *const *)newenv);
        sys_puts("env: execve failed\n");
        return 127;
    }

    /* No command: print environment (including any assignments). */
    if (!opt_i && envp) {
        for (int j = 0; envp[j]; j++) {
            (void)sys_write(1, envp[j], cstr_len_u64_local(envp[j]));
            (void)sys_write(1, "\n", 1);
        }
    }

    for (int j = 0; j < nassign; j++) {
        (void)sys_write(1, assign[j], cstr_len_u64_local(assign[j]));
        (void)sys_write(1, "\n", 1);
    }

    return 0;
}
