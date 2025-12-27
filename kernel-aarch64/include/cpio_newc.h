#pragma once

#include "stddef.h"
#include "stdint.h"

typedef struct {
    const char *name;            /* points into archive */
    uint32_t mode;
    const uint8_t *data;         /* points into archive */
    uint32_t size;
} cpio_entry_t;

typedef int (*cpio_iter_cb_t)(const cpio_entry_t *e, void *ctx);

/*
 * Finds an entry by name (exact match).
 * Returns 0 on success, -1 if not found or invalid archive.
 */
int cpio_newc_find(const void *archive, size_t archive_size, const char *name, cpio_entry_t *out);

/*
 * Iterate over all entries (excluding TRAILER!!!).
 * Callback returns 0 to continue, non-zero to stop.
 * Returns 0 if finished, -1 on parse error, or the callback's non-zero value.
 */
int cpio_newc_foreach(const void *archive, size_t archive_size, cpio_iter_cb_t cb, void *ctx);
