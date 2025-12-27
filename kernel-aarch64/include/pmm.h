#pragma once

#include "stdint.h"

typedef struct {
    uint64_t base;
    uint64_t size;
    uint64_t page_size;
    uint64_t total_pages;
    uint64_t free_pages;
} pmm_info_t;

void pmm_init(uint64_t mem_base, uint64_t mem_size, uint64_t kernel_start, uint64_t kernel_end, uint64_t dtb_ptr);
uint64_t pmm_alloc_page(void);          /* returns physical address, 0 on OOM */
void pmm_free_page(uint64_t pa);

/* Allocate a 2MiB-aligned contiguous physical region (512 * 4KiB pages).
 * Returns base physical address, or 0 on OOM.
 */
uint64_t pmm_alloc_2mib_aligned(void);

/* Free a region previously returned by pmm_alloc_2mib_aligned(). */
void pmm_free_2mib_aligned(uint64_t pa_base);

pmm_info_t pmm_info(void);
void pmm_dump(void);
