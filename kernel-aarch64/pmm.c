#include "pmm.h"
#include "uart_pl011.h"

#define PMM_PAGE_SIZE 4096ull

/* Phase 3: reserve a coarse 2MiB EL0 user sandbox region (identity-mapped). */
#define USER_REGION_BASE 0x00400000ull
#define USER_REGION_SIZE 0x00200000ull

/*
 * Simple bitmap PMM.
 *
 * - Manages a contiguous RAM range [base, base+size)
 * - 4KiB pages
 * - 1 bit per page: 0=free, 1=used
 *
 * For now this assumes a single RAM range (sufficient for QEMU raspi3b).
 */

/* Clamp to ~1GiB worth of pages for now (enough for raspi3b QEMU). */
#define PMM_MAX_PAGES  (262144ull) /* 262144 * 4096 = 1 GiB */
#define PMM_BITMAP_BYTES (PMM_MAX_PAGES / 8ull)

static uint8_t g_bitmap[PMM_BITMAP_BYTES];
static pmm_info_t g_info;

static inline uint64_t align_up_u64(uint64_t v, uint64_t a) {
    return (v + (a - 1)) & ~(a - 1);
}

static inline uint64_t align_down_u64(uint64_t v, uint64_t a) {
    return v & ~(a - 1);
}

static inline void bit_set(uint64_t idx) {
    g_bitmap[idx >> 3] |= (uint8_t)(1u << (idx & 7));
}

static inline void bit_clear(uint64_t idx) {
    g_bitmap[idx >> 3] &= (uint8_t)~(1u << (idx & 7));
}

static inline int bit_test(uint64_t idx) {
    return (g_bitmap[idx >> 3] >> (idx & 7)) & 1u;
}

static void bitmap_clear_all(uint64_t total_pages) {
    uint64_t bytes = (total_pages + 7) / 8;
    if (bytes > PMM_BITMAP_BYTES) {
        bytes = PMM_BITMAP_BYTES;
    }
    for (uint64_t i = 0; i < bytes; i++) {
        g_bitmap[i] = 0;
    }
}

static void bitmap_set_all(uint64_t total_pages) {
    uint64_t bytes = (total_pages + 7) / 8;
    if (bytes > PMM_BITMAP_BYTES) {
        bytes = PMM_BITMAP_BYTES;
    }
    for (uint64_t i = 0; i < bytes; i++) {
        g_bitmap[i] = 0xFF;
    }
}

static void reserve_range(uint64_t start, uint64_t end) {
    if (end <= start) {
        return;
    }

    uint64_t base = g_info.base;
    uint64_t limit = g_info.base + g_info.size;
    if (start < base) start = base;
    if (end > limit) end = limit;
    if (end <= start) {
        return;
    }

    uint64_t s = align_down_u64(start, PMM_PAGE_SIZE);
    uint64_t e = align_up_u64(end, PMM_PAGE_SIZE);

    for (uint64_t pa = s; pa < e; pa += PMM_PAGE_SIZE) {
        uint64_t idx = (pa - base) / PMM_PAGE_SIZE;
        if (idx >= g_info.total_pages) {
            break;
        }
        if (!bit_test(idx)) {
            bit_set(idx);
            if (g_info.free_pages > 0) {
                g_info.free_pages--;
            }
        }
    }
}

void pmm_init(uint64_t mem_base, uint64_t mem_size, uint64_t kernel_start, uint64_t kernel_end, uint64_t dtb_ptr) {
    /* Basic sanity */
    if (mem_size < PMM_PAGE_SIZE * 16) {
        uart_write("pmm: mem too small\n");
        bitmap_set_all(PMM_MAX_PAGES);
        g_info = (pmm_info_t){0};
        return;
    }

    uint64_t base = align_up_u64(mem_base, PMM_PAGE_SIZE);
    uint64_t end = align_down_u64(mem_base + mem_size, PMM_PAGE_SIZE);
    if (end <= base) {
        uart_write("pmm: bad mem range\n");
        bitmap_set_all(PMM_MAX_PAGES);
        g_info = (pmm_info_t){0};
        return;
    }

    uint64_t total_pages = (end - base) / PMM_PAGE_SIZE;
    if (total_pages > PMM_MAX_PAGES) {
        total_pages = PMM_MAX_PAGES;
        end = base + total_pages * PMM_PAGE_SIZE;
    }

    g_info = (pmm_info_t){
        .base = base,
        .size = end - base,
        .page_size = PMM_PAGE_SIZE,
        .total_pages = total_pages,
        .free_pages = total_pages,
    };

    bitmap_clear_all(total_pages);

    /* Reserve low memory conservatively (vectors/firmware/whatever). */
    reserve_range(base, base + 0x200000ull); /* 2 MiB */

    /* Reserve the kernel image range. */
    reserve_range(kernel_start, kernel_end);

    /* Reserve DTB blob region conservatively (64 KiB) around pointer. */
    if (dtb_ptr != 0) {
        reserve_range(dtb_ptr, dtb_ptr + 0x10000ull);
    }

    /* Reserve a user region for EL0 bring-up. */
    reserve_range(USER_REGION_BASE, USER_REGION_BASE + USER_REGION_SIZE);

    uart_write("pmm: initialized\n");
    pmm_dump();
}

uint64_t pmm_alloc_2mib_aligned(void) {
    /* 2MiB = 512 * 4KiB pages. Require 2MiB alignment. */
    const uint64_t pages = 512ull;
    if (g_info.free_pages < pages || g_info.total_pages == 0) {
        return 0;
    }

    for (uint64_t start = 0; start + pages <= g_info.total_pages; ) {
        /* Align candidate start index to 2MiB boundary in page units. */
        uint64_t aligned = (start + (pages - 1)) & ~(pages - 1);
        start = aligned;

        if (start + pages > g_info.total_pages) {
            break;
        }

        int ok = 1;
        for (uint64_t i = 0; i < pages; i++) {
            if (bit_test(start + i)) {
                ok = 0;
                break;
            }
        }

        if (ok) {
            for (uint64_t i = 0; i < pages; i++) {
                bit_set(start + i);
            }
            g_info.free_pages -= pages;
            return g_info.base + start * PMM_PAGE_SIZE;
        }

        /* Advance to next 2MiB boundary. */
        start += pages;
    }

    return 0;
}

void pmm_free_2mib_aligned(uint64_t pa_base) {
    if (pa_base == 0) {
        return;
    }
    if ((pa_base & 0x1FFFFFull) != 0) {
        return;
    }

    for (uint64_t i = 0; i < 512ull; i++) {
        pmm_free_page(pa_base + i * PMM_PAGE_SIZE);
    }
}

uint64_t pmm_alloc_page(void) {
    if (g_info.free_pages == 0 || g_info.total_pages == 0) {
        return 0;
    }

    for (uint64_t idx = 0; idx < g_info.total_pages; idx++) {
        if (!bit_test(idx)) {
            bit_set(idx);
            g_info.free_pages--;
            return g_info.base + idx * PMM_PAGE_SIZE;
        }
    }

    return 0;
}

void pmm_free_page(uint64_t pa) {
    if (g_info.total_pages == 0) {
        return;
    }
    if (pa < g_info.base || pa >= (g_info.base + g_info.size)) {
        return;
    }
    if ((pa & (PMM_PAGE_SIZE - 1)) != 0) {
        return;
    }

    uint64_t idx = (pa - g_info.base) / PMM_PAGE_SIZE;
    if (idx >= g_info.total_pages) {
        return;
    }

    if (bit_test(idx)) {
        bit_clear(idx);
        g_info.free_pages++;
    }
}

pmm_info_t pmm_info(void) {
    return g_info;
}

void pmm_dump(void) {
    uart_write("pmm: base=");
    uart_write_hex_u64(g_info.base);
    uart_write(" size=");
    uart_write_hex_u64(g_info.size);
    uart_write(" pages=");
    uart_write_hex_u64(g_info.total_pages);
    uart_write(" free=");
    uart_write_hex_u64(g_info.free_pages);
    uart_write("\n");
}
