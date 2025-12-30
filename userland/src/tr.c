#include "syscall.h"

enum {
    BUF_SZ = 512,
    SET_MAX = 512,
};

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

static void usage(void) {
    sys_puts("usage: tr SET1 SET2\n");
    sys_puts("       tr -d SET1\n");
}

static uint8_t hex_val(char c) {
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(10 + (c - 'a'));
    if (c >= 'A' && c <= 'F') return (uint8_t)(10 + (c - 'A'));
    return 0xff;
}

static int parse_escape(const char *s, uint64_t *i_inout, uint8_t *out) {
    uint64_t i = *i_inout;
    if (s[i] != '\\') return 0;
    i++;
    char c = s[i];
    if (c == '\0') return -1;

    if (c == 'n') {
        *out = (uint8_t)'\n';
        *i_inout = i + 1;
        return 1;
    }
    if (c == 't') {
        *out = (uint8_t)'\t';
        *i_inout = i + 1;
        return 1;
    }
    if (c == 'r') {
        *out = (uint8_t)'\r';
        *i_inout = i + 1;
        return 1;
    }
    if (c == '\\') {
        *out = (uint8_t)'\\';
        *i_inout = i + 1;
        return 1;
    }
    if (c == 'x') {
        uint8_t h1 = hex_val(s[i + 1]);
        uint8_t h2 = hex_val(s[i + 2]);
        if (h1 == 0xff || h2 == 0xff) return -1;
        *out = (uint8_t)((h1 << 4) | h2);
        *i_inout = i + 3;
        return 1;
    }

    /* Default: literal next character. */
    *out = (uint8_t)c;
    *i_inout = i + 1;
    return 1;
}

static int parse_set(const char *s, uint8_t *out, uint64_t out_cap, uint64_t *out_len) {
    if (!out_len) return -1;
    *out_len = 0;
    if (!s) return 0;

    uint64_t n = cstr_len_u64_local(s);
    uint64_t i = 0;
    while (i < n) {
        uint8_t a;
        if (s[i] == '\\') {
            uint64_t t = i;
            int erc = parse_escape(s, &t, &a);
            if (erc <= 0) return -1;
            i = t;
        } else {
            a = (uint8_t)s[i++];
        }

        /* Range expansion: a-b */
        if (i + 1 < n && s[i] == '-') {
            uint64_t j = i + 1;
            uint8_t b;
            if (s[j] == '\\') {
                uint64_t t = j;
                int erc = parse_escape(s, &t, &b);
                if (erc <= 0) return -1;
                j = t;
            } else {
                b = (uint8_t)s[j++];
            }

            i = j;

            if (a <= b) {
                for (uint16_t v = a; v <= (uint16_t)b; v++) {
                    if (*out_len >= out_cap) return -1;
                    out[(*out_len)++] = (uint8_t)v;
                }
            } else {
                for (int v = (int)a; v >= (int)b; v--) {
                    if (*out_len >= out_cap) return -1;
                    out[(*out_len)++] = (uint8_t)v;
                }
            }
            continue;
        }

        if (*out_len >= out_cap) return -1;
        out[(*out_len)++] = a;
    }

    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    int delete_mode = 0;
    const char *set1 = 0;
    const char *set2 = 0;

    if (argc == 3 && streq(argv[1], "-d")) {
        delete_mode = 1;
        set1 = argv[2];
    } else if (argc == 3) {
        set1 = argv[1];
        set2 = argv[2];
    } else {
        usage();
        return 1;
    }

    uint8_t s1[SET_MAX];
    uint64_t s1_len = 0;
    if (parse_set(set1, s1, sizeof(s1), &s1_len) != 0) {
        sys_puts("tr: invalid SET1\n");
        return 1;
    }

    uint8_t map[256];
    uint8_t del[256];
    for (uint64_t i = 0; i < 256; i++) {
        map[i] = (uint8_t)i;
        del[i] = 0;
    }

    if (delete_mode) {
        for (uint64_t i = 0; i < s1_len; i++) {
            del[s1[i]] = 1;
        }
    } else {
        uint8_t s2[SET_MAX];
        uint64_t s2_len = 0;
        if (parse_set(set2, s2, sizeof(s2), &s2_len) != 0 || s2_len == 0) {
            sys_puts("tr: invalid SET2\n");
            return 1;
        }
        for (uint64_t i = 0; i < s1_len; i++) {
            uint8_t from = s1[i];
            uint8_t to = s2[(i < s2_len) ? i : (s2_len - 1)];
            map[from] = to;
        }
    }

    char in[BUF_SZ];
    char out[BUF_SZ];
    for (;;) {
        int64_t n = (int64_t)sys_read(0, in, sizeof(in));
        if (n < 0) return 1;
        if (n == 0) break;

        uint64_t o = 0;
        for (int64_t i = 0; i < n; i++) {
            uint8_t b = (uint8_t)in[i];
            uint8_t t = map[b];
            if (del[t]) continue;
            out[o++] = (char)t;
        }
        if (o > 0) {
            int64_t w = (int64_t)sys_write(1, out, o);
            if (w < 0) return 1;
        }
    }

    return 0;
}
