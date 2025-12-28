#include "vfs.h"

#include "stdint.h"

/* Keep these consistent with the rest of the kernel (exceptions.c). */
#define ENOMEM 12
#define ENAMETOOLONG 36
#define EEXIST 17
#define ENOENT 2
#define ENOTDIR 20

enum {
    MAX_PATH = 256,
    MAX_RAMDIRS = 64,
};

typedef struct {
    uint8_t used;
    uint32_t mode;       /* includes S_IFDIR */
    char path[MAX_PATH]; /* normalized, no leading slashes, no trailing slash (except "" for root, unused) */
} ramdir_t;

static ramdir_t g_ramdirs[MAX_RAMDIRS];

static uint64_t cstr_len_u64(const char *s) {
    uint64_t n = 0;
    while (s && s[n] != '\0') n++;
    return n;
}

static uint32_t cstr_len_u32(const char *s) {
    uint32_t n = 0;
    while (s && s[n] != '\0') n++;
    return n;
}

static int cstr_eq_u64(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static const char *strip_leading_slashes_const(const char *p) {
    while (p && *p == '/') p++;
    return p;
}

static int ramdir_find(const char *path_no_slash) {
    for (int i = 0; i < (int)MAX_RAMDIRS; i++) {
        if (!g_ramdirs[i].used) continue;
        if (cstr_eq_u64(g_ramdirs[i].path, path_no_slash)) return i;
    }
    return -1;
}

static int ramdir_alloc_slot(void) {
    for (int i = 0; i < (int)MAX_RAMDIRS; i++) {
        if (!g_ramdirs[i].used) return i;
    }
    return -1;
}

void vfs_init(void) {
    for (uint64_t i = 0; i < (uint64_t)MAX_RAMDIRS; i++) {
        g_ramdirs[i].used = 0;
        g_ramdirs[i].mode = 0;
        g_ramdirs[i].path[0] = '\0';
    }
}

int vfs_lookup_abs(const char *abs_path, const uint8_t **out_data, uint64_t *out_size, uint32_t *out_mode) {
    if (!abs_path) return -1;

    /* Root directory always exists. */
    if (cstr_eq_u64(abs_path, "/")) {
        if (out_data) *out_data = 0;
        if (out_size) *out_size = 0;
        if (out_mode) *out_mode = 0040000u | 0755u;
        return 0;
    }

    const char *p = strip_leading_slashes_const(abs_path);
    if (!p || *p == '\0') {
        if (out_data) *out_data = 0;
        if (out_size) *out_size = 0;
        if (out_mode) *out_mode = 0040000u | 0755u;
        return 0;
    }

    int ridx = ramdir_find(p);
    if (ridx >= 0) {
        if (out_data) *out_data = 0;
        if (out_size) *out_size = 0;
        if (out_mode) *out_mode = g_ramdirs[ridx].mode;
        return 0;
    }

    if (initramfs_lookup(abs_path, out_data, out_size, out_mode) == 0) {
        return 0;
    }
    return -1;
}

typedef struct {
    initramfs_dir_cb_t cb;
    void *cb_ctx;
    const char *seen[128];
    uint32_t seen_len[128];
    uint32_t seen_count;
} vfs_list_ctx_t;

static int slice_eq_u32(const char *a, uint32_t alen, const char *b, uint32_t blen) {
    if (alen != blen) return 0;
    for (uint32_t i = 0; i < alen; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

static int vfs_seen_has(vfs_list_ctx_t *vc, const char *name, uint32_t len) {
    for (uint32_t i = 0; i < vc->seen_count; i++) {
        if (slice_eq_u32(vc->seen[i], vc->seen_len[i], name, len)) return 1;
    }
    return 0;
}

static void vfs_seen_add(vfs_list_ctx_t *vc, const char *name, uint32_t len) {
    if (vc->seen_count >= (uint32_t)(sizeof(vc->seen) / sizeof(vc->seen[0]))) return;
    vc->seen[vc->seen_count] = name;
    vc->seen_len[vc->seen_count] = len;
    vc->seen_count++;
}

static int vfs_list_emit_unique(const char *name, uint32_t mode, void *ctx) {
    vfs_list_ctx_t *vc = (vfs_list_ctx_t *)ctx;
    uint32_t n = cstr_len_u32(name);
    if (n == 0) return 0;
    if (vfs_seen_has(vc, name, n)) return 0;
    vfs_seen_add(vc, name, n);
    return vc->cb(name, mode, vc->cb_ctx);
}

int vfs_list_dir(const char *dir_path_no_slash, initramfs_dir_cb_t cb, void *ctx) {
    if (!cb) return -1;

    /* Ensure dir exists (either initramfs or ramdir). */
    char abs[MAX_PATH];
    if (!dir_path_no_slash || dir_path_no_slash[0] == '\0') {
        abs[0] = '/';
        abs[1] = '\0';
    } else {
        uint64_t n = cstr_len_u64(dir_path_no_slash);
        if (n + 2 > sizeof(abs)) return -1;
        abs[0] = '/';
        for (uint64_t i = 0; i <= n; i++) abs[1 + i] = dir_path_no_slash[i];
    }

    uint32_t mode = 0;
    if (vfs_lookup_abs(abs, 0, 0, &mode) != 0) return -1;
    if ((mode & 0170000u) != 0040000u) return -1;

    vfs_list_ctx_t vc;
    vc.cb = cb;
    vc.cb_ctx = ctx;
    vc.seen_count = 0;

    /* First: initramfs entries. */
    (void)initramfs_list_dir(dir_path_no_slash ? dir_path_no_slash : "", vfs_list_emit_unique, &vc);

    /* Second: ramdir overlay entries. */
    const char *prefix = dir_path_no_slash ? dir_path_no_slash : "";
    uint32_t plen = cstr_len_u32(prefix);

    for (int i = 0; i < (int)MAX_RAMDIRS; i++) {
        if (!g_ramdirs[i].used) continue;

        const char *rp = g_ramdirs[i].path;
        if (!rp || rp[0] == '\0') continue;

        const char *child = rp;
        if (plen != 0) {
            /* Must start with "<prefix>/" */
            for (uint32_t j = 0; j < plen; j++) {
                if (rp[j] != prefix[j]) {
                    child = 0;
                    break;
                }
            }
            if (!child) continue;
            if (rp[plen] != '/') continue;
            child = rp + plen + 1;
            if (child[0] == '\0') continue; /* the directory itself */
        }

        uint32_t clen = 0;
        while (child[clen] != '\0' && child[clen] != '/') clen++;
        if (clen == 0) continue;

        /* Determine mode for this immediate child: look up exact child path if present. */
        uint32_t child_mode = 0040000u | 0755u;
        {
            char child_full[MAX_PATH];
            uint64_t o = 0;
            if (plen != 0) {
                if ((uint64_t)plen + 1 >= sizeof(child_full)) {
                    continue;
                }
                for (uint32_t j = 0; j < plen; j++) child_full[o++] = prefix[j];
                child_full[o++] = '/';
            }
            if (o + (uint64_t)clen + 1 >= sizeof(child_full)) {
                continue;
            }
            for (uint32_t j = 0; j < clen; j++) child_full[o++] = child[j];
            child_full[o] = '\0';

            int ei = ramdir_find(child_full);
            if (ei >= 0) child_mode = g_ramdirs[ei].mode;
        }

        /* Copy into temporary NUL-terminated buffer for callback. */
        char tmp[128];
        if (clen >= sizeof(tmp)) clen = (uint32_t)sizeof(tmp) - 1;
        for (uint32_t j = 0; j < clen; j++) tmp[j] = child[j];
        tmp[clen] = '\0';

        int rc = vfs_list_emit_unique(tmp, child_mode, &vc);
        if (rc != 0) return rc;
    }

    return 0;
}

int vfs_ramdir_create(const char *path_no_slash, uint32_t mode) {
    if (!path_no_slash || path_no_slash[0] == '\0') {
        return -(int)ENOENT;
    }
    if (ramdir_find(path_no_slash) >= 0) {
        return -(int)EEXIST;
    }

    int slot = ramdir_alloc_slot();
    if (slot < 0) {
        return -(int)ENOMEM;
    }

    uint64_t n = cstr_len_u64(path_no_slash);
    if (n + 1 > sizeof(g_ramdirs[slot].path)) {
        return -(int)ENAMETOOLONG;
    }

    g_ramdirs[slot].used = 1;
    g_ramdirs[slot].mode = mode;
    for (uint64_t i = 0; i <= n; i++) g_ramdirs[slot].path[i] = path_no_slash[i];

    return 0;
}
