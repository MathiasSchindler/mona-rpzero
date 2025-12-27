#pragma once

#include "stdint.h"

/*
 * Minimal Flattened Device Tree (FDT/DTB) reader.
 *
 * Goal: just enough for bring-up diagnostics:
 *  - verify DTB header
 *  - print /model
 *  - extract first RAM range from a memory node's reg property
 */

typedef struct {
    const char *model; /* points into DTB blob */
    uint64_t mem_base;
    uint64_t mem_size;
    int has_model;
    int has_mem;
} fdt_info_t;

int fdt_read_info(const void *dtb, fdt_info_t *out);
void fdt_print_info(const void *dtb);
