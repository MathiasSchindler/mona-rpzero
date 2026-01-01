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
    sys_puts("usage: dirname PATH...\n");
}

static void print_dirname(const char *path) {
    if (!path || path[0] == '\0') {
        sys_puts(".\n");
        return;
    }

    uint64_t n = cstr_len_u64_local(path);

    /* Strip trailing slashes (but keep a single leading '/'). */
    while (n > 1 && path[n - 1] == '/') n--;

    /* If the entire path is just slashes, result is '/'. */
    int all_slash = 1;
    for (uint64_t i = 0; i < n; i++) {
        if (path[i] != '/') {
            all_slash = 0;
            break;
        }
    }
    if (all_slash) {
        sys_puts("/\n");
        return;
    }

    /* Find last '/' within trimmed prefix. */
    int64_t last = -1;
    for (uint64_t i = 0; i < n; i++) {
        if (path[i] == '/') last = (int64_t)i;
    }

    if (last < 0) {
        sys_puts(".\n");
        return;
    }

    /* Strip any additional slashes before last component. */
    uint64_t end = (uint64_t)last;
    while (end > 1 && path[end - 1] == '/') end--;

    if (end == 0) {
        sys_puts("/\n");
        return;
    }

    (void)sys_write(1, path, end);
    (void)sys_write(1, "\n", 1);
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    if (argc >= 2 && argv[1] && (streq(argv[1], "-h") || streq(argv[1], "--help"))) {
        usage();
        return 0;
    }

    if (argc < 2) {
        /* Friendly behavior: dirname with no args => "." */
        sys_puts(".\n");
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        print_dirname(argv[i]);
    }

    return 0;
}
