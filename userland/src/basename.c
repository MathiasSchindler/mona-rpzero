#include "syscall.h"

static int streq(const char *a, const char *b) {
    if (!a || !b) return 0;
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
    sys_puts("usage: basename NAME [SUFFIX]\n");
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    if (argc < 2) {
        usage();
        return 1;
    }

    const char *name = argv[1];
    if (!name || name[0] == '\0') {
        sys_puts(".\n");
        return 0;
    }

    uint64_t len = cstr_len_u64_local(name);

    /* If it's all slashes, basename is "/". */
    int all_slash = 1;
    for (uint64_t i = 0; i < len; i++) {
        if (name[i] != '/') {
            all_slash = 0;
            break;
        }
    }
    if (all_slash) {
        sys_puts("/\n");
        return 0;
    }

    /* Strip trailing slashes (but not down to empty). */
    while (len > 0 && name[len - 1] == '/') {
        len--;
    }
    if (len == 0) {
        sys_puts("/\n");
        return 0;
    }

    /* Find start after last '/'. */
    uint64_t start = 0;
    for (uint64_t i = 0; i < len; i++) {
        if (name[i] == '/') start = i + 1;
    }

    uint64_t base_len = len - start;

    /* Optional suffix removal. */
    const char *suf = (argc >= 3) ? argv[2] : 0;
    if (suf && suf[0] != '\0') {
        uint64_t suf_len = cstr_len_u64_local(suf);
        if (suf_len > 0 && suf_len < base_len) {
            int match = 1;
            for (uint64_t i = 0; i < suf_len; i++) {
                if (name[start + base_len - suf_len + i] != suf[i]) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                base_len -= suf_len;
            }
        }
    }

    (void)streq; /* keep pattern consistent; may be useful later */

    (void)sys_write(1, name + start, base_len);
    sys_puts("\n");
    return 0;
}
