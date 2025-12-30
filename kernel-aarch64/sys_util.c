#include "sys_util.h"

#include "errno.h"
#include "mmu.h"

int user_range_ok(uint64_t user_ptr, uint64_t len) {
    if (len == 0) return 1;
    if (user_ptr < USER_REGION_BASE) return 0;
    if (user_ptr >= USER_REGION_BASE + USER_REGION_SIZE) return 0;
    if (user_ptr + len < user_ptr) return 0; /* overflow */
    if (user_ptr + len > USER_REGION_BASE + USER_REGION_SIZE) return 0;
    return 1;
}

uint64_t cstr_len_u64(const char *s) {
    uint64_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}

uint64_t cstr_len(const char *s) {
    uint64_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}

static int cstr_starts_with(const char *s, char c) {
    return s[0] == c;
}

int cstr_eq_u64(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

int normalize_abs_path(const char *in, char *out, uint64_t outsz) {
    if (outsz < 2) return -1;

    uint64_t seg_slash[64];
    uint64_t depth = 0;
    uint64_t o = 0;
    out[0] = '\0';

    const char *p = in;
    while (*p != '\0') {
        while (*p == '/') p++;
        if (*p == '\0') break;

        const char *seg = p;
        uint64_t seglen = 0;
        while (*p != '\0' && *p != '/') {
            p++;
            seglen++;
        }

        if (seglen == 1 && seg[0] == '.') {
            continue;
        }
        if (seglen == 2 && seg[0] == '.' && seg[1] == '.') {
            if (depth > 0) {
                o = seg_slash[--depth];
                out[o] = '\0';
            }
            continue;
        }

        if (o + 1 >= outsz) return -1;
        out[o++] = '/';
        if (depth < (uint64_t)(sizeof(seg_slash) / sizeof(seg_slash[0]))) {
            seg_slash[depth++] = o - 1;
        } else {
            return -1;
        }

        if (o + seglen >= outsz) return -1;
        for (uint64_t i = 0; i < seglen; i++) {
            out[o++] = seg[i];
        }
        out[o] = '\0';
    }

    if (o == 0) {
        out[0] = '/';
        out[1] = '\0';
    }
    return 0;
}

int resolve_path(proc_t *p, const char *in, char *out, uint64_t outsz) {
    if (!p || !in || !out) return -1;

    char tmp[MAX_PATH];
    tmp[0] = '\0';

    if (cstr_starts_with(in, '/')) {
        /* Already absolute. */
        uint64_t n = cstr_len_u64(in);
        if (n + 1 > sizeof(tmp)) return -1;
        for (uint64_t i = 0; i <= n; i++) tmp[i] = in[i];
    } else {
        uint64_t cwd_len = cstr_len_u64(p->cwd);
        uint64_t in_len = cstr_len_u64(in);

        /* tmp = cwd + "/" + in, allowing cwd=="/". */
        if (cwd_len == 0) {
            return -1;
        }

        uint64_t need = cwd_len + 1 + in_len + 1;
        if (need > sizeof(tmp)) return -1;

        uint64_t o = 0;
        for (uint64_t i = 0; i < cwd_len; i++) tmp[o++] = p->cwd[i];
        tmp[o++] = '/';
        for (uint64_t i = 0; i < in_len; i++) tmp[o++] = in[i];
        tmp[o] = '\0';
    }

    return normalize_abs_path(tmp, out, outsz);
}

int abs_path_to_no_slash_trim(const char *abs_path, char *out_no_slash, uint64_t outsz) {
    if (!abs_path || !out_no_slash || outsz == 0) return -(int)EINVAL;

    const char *p = abs_path;
    while (*p == '/') p++;
    if (*p == '\0') {
        if (outsz > 0) out_no_slash[0] = '\0';
        return -(int)EINVAL;
    }

    uint64_t n = cstr_len_u64(p);
    if (n + 1 > outsz) return -(int)ENAMETOOLONG;
    for (uint64_t i = 0; i <= n; i++) out_no_slash[i] = p[i];

    while (n > 0 && out_no_slash[n - 1] == '/') {
        out_no_slash[n - 1] = '\0';
        n--;
    }
    if (n == 0) return -(int)EINVAL;
    return 0;
}

int abs_path_parent_dir(const char *abs_path, char *out_parent_abs, uint64_t outsz) {
    if (!abs_path || !out_parent_abs || outsz < 2) return -(int)EINVAL;

    uint64_t n = cstr_len_u64(abs_path);
    if (n + 1 > outsz) return -(int)ENAMETOOLONG;
    for (uint64_t i = 0; i <= n; i++) out_parent_abs[i] = abs_path[i];

    /* Trim trailing slashes but keep "/" as-is. */
    while (n > 1 && out_parent_abs[n - 1] == '/') {
        out_parent_abs[n - 1] = '\0';
        n--;
    }

    int last = -1;
    for (uint64_t i = 0; out_parent_abs[i] != '\0'; i++) {
        if (out_parent_abs[i] == '/') last = (int)i;
    }
    if (last < 0) return -(int)EINVAL;

    if (last == 0) {
        out_parent_abs[0] = '/';
        out_parent_abs[1] = '\0';
        return 0;
    }

    out_parent_abs[last] = '\0';
    return 0;
}

int copy_cstr_from_user(char *dst, uint64_t dstsz, uint64_t user_ptr) {
    if (dstsz == 0) return -1;
    for (uint64_t i = 0; i < dstsz; i++) {
        if (!user_range_ok(user_ptr + i, 1)) return -1;
        char c = *(const volatile char *)(uintptr_t)(user_ptr + i);
        dst[i] = c;
        if (c == '\0') return 0;
    }
    dst[dstsz - 1] = '\0';
    return -1;
}

int read_u64_from_user(uint64_t user_ptr, uint64_t *out) {
    if (!user_range_ok(user_ptr, 8)) return -1;
    *out = *(const volatile uint64_t *)(uintptr_t)user_ptr;
    return 0;
}

int write_bytes_to_user(uint64_t user_dst, const void *src, uint64_t len) {
    if (!user_range_ok(user_dst, len)) return -1;
    volatile uint8_t *d = (volatile uint8_t *)(uintptr_t)user_dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < len; i++) d[i] = s[i];
    return 0;
}

int write_u64_to_user(uint64_t user_dst, uint64_t v) {
    if (!user_range_ok(user_dst, 8)) return -1;
    *(volatile uint64_t *)(uintptr_t)user_dst = v;
    return 0;
}

uint64_t align_down_u64(uint64_t x, uint64_t a) {
    return x & ~(a - 1u);
}

uint64_t align_up_u64(uint64_t x, uint64_t a) {
    return (x + (a - 1u)) & ~(a - 1u);
}
