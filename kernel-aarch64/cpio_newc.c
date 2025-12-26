#include "cpio_newc.h"

static int is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static uint32_t hex8_to_u32(const char *p) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        char c = p[i];
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(10 + (c - 'a'));
        else d = (uint32_t)(10 + (c - 'A'));
        v = (v << 4) | d;
    }
    return v;
}

static size_t pad4(size_t n) {
    return (4 - (n & 3u)) & 3u;
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int parse_one(const uint8_t **pp, const uint8_t *end, cpio_entry_t *out) {
    const uint8_t *p = *pp;
    if ((size_t)(end - p) < 110) return -1;

    const char *h = (const char *)p;
    if (h[0] != '0' || h[1] != '7' || h[2] != '0' || h[3] != '7' || h[4] != '0' || h[5] != '1') {
        return -1;
    }
    for (int i = 6; i < 110; i++) {
        if (!is_hex(h[i])) return -1;
    }

    uint32_t mode = hex8_to_u32(h + 14);
    uint32_t filesize = hex8_to_u32(h + 54);
    uint32_t namesize = hex8_to_u32(h + 94);

    p += 110;
    if ((size_t)(end - p) < namesize) return -1;

    const char *entry_name = (const char *)p;
    if (namesize == 0) return -1;
    if (entry_name[namesize - 1] != '\0') return -1;

    p += namesize;
    p += pad4(110 + (size_t)namesize);

    if ((size_t)(end - p) < filesize) return -1;

    const uint8_t *data = p;
    p += filesize;
    p += pad4((size_t)filesize);

    if (out) {
        out->name = entry_name;
        out->mode = mode;
        out->data = data;
        out->size = filesize;
    }

    *pp = p;
    return 0;
}

int cpio_newc_find(const void *archive, size_t archive_size, const char *name, cpio_entry_t *out) {
    const uint8_t *p = (const uint8_t *)archive;
    const uint8_t *end = p + archive_size;

    for (;;) {
        cpio_entry_t e;
        if (parse_one(&p, end, &e) != 0) return -1;

        const char *entry_name = e.name;
        if (str_eq(entry_name, "TRAILER!!!")) {
            return -1;
        }

        if (str_eq(entry_name, name)) {
            if (out) {
                *out = e;
            }
            return 0;
        }

        if (p >= end) return -1;
    }
}

int cpio_newc_foreach(const void *archive, size_t archive_size, cpio_iter_cb_t cb, void *ctx) {
    const uint8_t *p = (const uint8_t *)archive;
    const uint8_t *end = p + archive_size;

    for (;;) {
        cpio_entry_t e;
        if (parse_one(&p, end, &e) != 0) return -1;
        if (str_eq(e.name, "TRAILER!!!")) return 0;

        int rc = cb(&e, ctx);
        if (rc != 0) return rc;

        if (p >= end) return -1;
    }
}
