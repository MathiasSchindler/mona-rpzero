#include "initramfs.h"
#include "cpio_newc.h"
#include "stat_bits.h"

static const void *g_archive;
static size_t g_archive_size;

static void strip_leading_slashes(const char **p) {
    while (**p == '/') (*p)++;
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static uint32_t root_dir_mode(void) {
    /* S_IFDIR | 0755 */
    return S_IFDIR | 0755u;
}

void initramfs_init(const void *archive, size_t archive_size) {
    g_archive = archive;
    g_archive_size = archive_size;
}

int initramfs_lookup(const char *path, const uint8_t **out_data, uint64_t *out_size, uint32_t *out_mode) {
    if (!g_archive || g_archive_size == 0) return -1;

    /* Treat "/" as the root directory. */
    if (str_eq(path, "/")) {
        if (out_data) *out_data = 0;
        if (out_size) *out_size = 0;
        if (out_mode) *out_mode = root_dir_mode();
        return 0;
    }

    strip_leading_slashes(&path);
    if (*path == '\0') {
        if (out_data) *out_data = 0;
        if (out_size) *out_size = 0;
        if (out_mode) *out_mode = root_dir_mode();
        return 0;
    }

    cpio_entry_t e;
    if (cpio_newc_find(g_archive, g_archive_size, path, &e) != 0) return -1;

    if (out_data) *out_data = e.data;
    if (out_size) *out_size = (uint64_t)e.size;
    if (out_mode) *out_mode = e.mode;
    return 0;
}

typedef struct {
    const char *dir;      /* normalized, no leading slashes, may be "" for root */
    uint32_t dir_len;
    initramfs_dir_cb_t cb;
    void *cb_ctx;
    const char *seen[64];
    uint32_t seen_len[64];
    uint32_t seen_count;
} list_ctx_t;

static uint32_t cstr_len(const char *s) {
    uint32_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}

static int slice_eq(const char *a, uint32_t alen, const char *b, uint32_t blen) {
    if (alen != blen) return 0;
    for (uint32_t i = 0; i < alen; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

static int seen_has(list_ctx_t *lc, const char *name, uint32_t len) {
    for (uint32_t i = 0; i < lc->seen_count; i++) {
        if (slice_eq(lc->seen[i], lc->seen_len[i], name, len)) return 1;
    }
    return 0;
}

static void seen_add(list_ctx_t *lc, const char *name, uint32_t len) {
    if (lc->seen_count >= (uint32_t)(sizeof(lc->seen) / sizeof(lc->seen[0]))) return;
    lc->seen[lc->seen_count] = name;
    lc->seen_len[lc->seen_count] = len;
    lc->seen_count++;
}

static uint32_t infer_child_mode(const cpio_entry_t *e, uint32_t child_is_dir) {
    if (child_is_dir) {
        return S_IFDIR | 0755u;
    }
    return e->mode;
}

static int list_cb(const cpio_entry_t *e, void *ctx) {
    list_ctx_t *lc = (list_ctx_t *)ctx;
    const char *name = e->name;

    if (name[0] == '\0') return 0;

    const char *child = name;
    uint32_t child_len = 0;
    uint32_t child_is_dir = 0;

    if (lc->dir_len != 0) {
        for (uint32_t i = 0; i < lc->dir_len; i++) {
            if (name[i] != lc->dir[i]) return 0;
        }
        if (name[lc->dir_len] != '/') return 0;
        child = name + lc->dir_len + 1;
        if (child[0] == '\0') return 0;
    }

    while (child[child_len] != '\0' && child[child_len] != '/') {
        child_len++;
    }
    if (child_len == 0) return 0;

    if (child[child_len] == '/') child_is_dir = 1;

    if (seen_has(lc, child, child_len)) return 0;
    seen_add(lc, child, child_len);

    char tmp[128];
    if (child_len >= sizeof(tmp)) child_len = (uint32_t)sizeof(tmp) - 1;
    for (uint32_t i = 0; i < child_len; i++) tmp[i] = child[i];
    tmp[child_len] = '\0';

    uint32_t mode = infer_child_mode(e, child_is_dir);
    return lc->cb(tmp, mode, lc->cb_ctx);
}

int initramfs_list_dir(const char *dir_path, initramfs_dir_cb_t cb, void *ctx) {
    if (!g_archive || g_archive_size == 0) return -1;
    if (!cb) return -1;

    if (str_eq(dir_path, "/")) dir_path = "";
    strip_leading_slashes(&dir_path);

    list_ctx_t lc;
    lc.dir = dir_path;
    lc.dir_len = cstr_len(dir_path);
    lc.cb = cb;
    lc.cb_ctx = ctx;
    lc.seen_count = 0;

    int rc = cpio_newc_foreach(g_archive, g_archive_size, list_cb, &lc);
    if (rc == 0) return 0;
    return -1;
}
