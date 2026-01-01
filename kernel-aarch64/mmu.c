#include "mmu.h"
#include "uart_pl011.h"
#include "pmm.h"
#include "cache.h"

/*
 * 4KB granule, 39-bit VA (T0SZ=25) with a single L0 entry.
 * With 4KB granule and T0SZ=25 (39-bit VA), TTBR0/TTBR1 each point to a Level-1 table.
 * We build:
 *   L1 -> L2
 * and use 2MB block mappings at Level-2.
 */

#define PAGE_SIZE 4096ull
#define TABLE_ENTRIES 512ull

#define PERIPH_BASE 0x3F000000ull
/* Include the BCM2836/BCM2710 "local peripherals" window at 0x4000_0000. */
#define PERIPH_END  0x40001000ull

/* Descriptor bits */
#define DESC_VALID   (1ull << 0)
#define DESC_TABLE   (1ull << 1) /* for table descriptors: 0b11 */
#define DESC_BLOCK   (0ull << 1) /* for block descriptors: 0b01 */

#define PTE_TYPE_TABLE  (0b11ull)
#define PTE_TYPE_BLOCK  (0b01ull)

#define PTE_AF      (1ull << 10)

/* SH[9:8] */
#define PTE_SH_SHIFT 8
#define PTE_SH_NONE  (0ull << PTE_SH_SHIFT)
#define PTE_SH_OUTER (2ull << PTE_SH_SHIFT)
#define PTE_SH_INNER (3ull << PTE_SH_SHIFT)

/* AP[7:6] */
#define PTE_AP_SHIFT 6
#define PTE_AP_RW_EL1 (0ull << PTE_AP_SHIFT)
#define PTE_AP_RW_EL0 (1ull << PTE_AP_SHIFT)

/* AttrIndx[4:2] */
#define PTE_ATTR_SHIFT 2
#define PTE_ATTR(idx) ((uint64_t)(idx) << PTE_ATTR_SHIFT)

#define PTE_PXN (1ull << 53)
#define PTE_UXN (1ull << 54)

/* MAIR attribute indices */
#define ATTR_NORMAL 0
#define ATTR_DEVICE 1

static uint64_t *g_l2_template0 = 0;
static uint64_t *g_l2_template1 = 0;

static inline uint64_t align_down(uint64_t v, uint64_t a);
static inline uint64_t align_up(uint64_t v, uint64_t a);

static uint64_t *alloc_table_page(void);
static uint64_t make_table_desc(uint64_t next_table_pa);
static uint64_t make_block_desc(uint64_t out_pa, int attr_index, uint64_t ap, int is_device);

static inline uint64_t read_sctlr_el1(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(v));
    return v;
}

static inline void write_sctlr_el1(uint64_t v) {
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(v));
    __asm__ volatile("isb");
}

static inline uint64_t read_tcr_el1(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, tcr_el1" : "=r"(v));
    return v;
}

static inline void write_tcr_el1(uint64_t v) {
    __asm__ volatile("msr tcr_el1, %0" :: "r"(v));
    __asm__ volatile("isb");
}

static inline void write_mair_el1(uint64_t v) {
    __asm__ volatile("msr mair_el1, %0" :: "r"(v));
    __asm__ volatile("isb");
}

static inline void write_ttbr0_el1(uint64_t v) {
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"(v));
    __asm__ volatile("isb");
}

static inline void write_ttbr1_el1(uint64_t v) {
    __asm__ volatile("msr ttbr1_el1, %0" :: "r"(v));
    __asm__ volatile("isb");
}

static inline void tlbi_vmalle1(void) {
    __asm__ volatile("dsb ish");
    __asm__ volatile("tlbi vmalle1");
    __asm__ volatile("dsb ish");
    __asm__ volatile("isb");
}

int mmu_mark_region_device(uint64_t phys_start, uint64_t size_bytes) {
    if (!mmu_is_enabled()) {
        return -1;
    }
    if (!g_l2_template0) {
        return -1;
    }
    if (size_bytes == 0) {
        return -1;
    }

    /* We only have L2 entries for VA/PA 0..1GiB in 2MiB blocks. */
    uint64_t start = align_down(phys_start, 0x200000ull);
    uint64_t end = align_up(phys_start + size_bytes, 0x200000ull);
    if (end <= start) {
        return -1;
    }

    __asm__ volatile("dsb ish");
    for (uint64_t pa = start; pa < end; pa += 0x200000ull) {
        uint64_t idx = pa / 0x200000ull;
        if (idx >= TABLE_ENTRIES) {
            return -1;
        }
        g_l2_template0[idx] = make_block_desc(pa, ATTR_DEVICE, PTE_AP_RW_EL1, /*is_device=*/1);
    }

    tlbi_vmalle1();
    return 0;
}

uint64_t mmu_ttbr0_read(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(v));
    return v;
}

void mmu_ttbr0_write(uint64_t ttbr0_pa) {
    write_ttbr0_el1(ttbr0_pa);
    tlbi_vmalle1();
}

uint64_t mmu_ttbr0_create_with_user_pa(uint64_t user_pa_base) {
    if (!g_l2_template0) {
        return 0;
    }
    if ((user_pa_base & 0x1FFFFFull) != 0) {
        return 0; /* must be 2MiB-aligned (we map a 2MiB block) */
    }

    uint64_t *l1 = alloc_table_page();
    uint64_t *l2_0 = alloc_table_page();
    uint64_t *l2_1 = alloc_table_page();
    if (!l1 || !l2_0 || !l2_1) {
        return 0;
    }

    /* Clone template L2[0..1GiB). */
    for (uint64_t i = 0; i < TABLE_ENTRIES; i++) {
        l2_0[i] = g_l2_template0[i];
    }

    /* Clone template L2[1GiB..2GiB). */
    if (g_l2_template1) {
        for (uint64_t i = 0; i < TABLE_ENTRIES; i++) {
            l2_1[i] = g_l2_template1[i];
        }
    }

    /* Override the user block mapping to point at the chosen physical base.
     * Keep it RW for EL0.
     */
    const uint64_t user_idx = (USER_REGION_BASE >> 21) & 0x1FFu;
    l2_0[user_idx] = make_block_desc(user_pa_base, ATTR_NORMAL, PTE_AP_RW_EL0, 0);

    l1[0] = make_table_desc((uint64_t)(uintptr_t)l2_0);
    l1[1] = make_table_desc((uint64_t)(uintptr_t)l2_1);
    return (uint64_t)(uintptr_t)l1;
}

static inline uint64_t align_down(uint64_t v, uint64_t a) {
    return v & ~(a - 1);
}

static inline uint64_t align_up(uint64_t v, uint64_t a) {
    return (v + (a - 1)) & ~(a - 1);
}

static uint64_t *alloc_table_page(void) {
    uint64_t pa = pmm_alloc_page();
    if (pa == 0) {
        return 0;
    }
    /* Identity-mapped for now: VA==PA */
    uint64_t *va = (uint64_t *)(uintptr_t)pa;

    for (uint64_t i = 0; i < TABLE_ENTRIES; i++) {
        va[i] = 0;
    }
    return va;
}

static uint64_t make_table_desc(uint64_t next_table_pa) {
    return (next_table_pa & 0x0000FFFFFFFFF000ull) | PTE_TYPE_TABLE;
}

static uint64_t make_block_desc(uint64_t out_pa, int attr_index, uint64_t ap, int is_device) {
    uint64_t desc = 0;
    desc |= (out_pa & 0x0000FFFFFFE00000ull); /* 2MB block base */
    desc |= PTE_TYPE_BLOCK;
    desc |= PTE_AF;
    desc |= ap;
    desc |= PTE_ATTR(attr_index);

    if (is_device) {
        desc |= PTE_SH_NONE;
        desc |= PTE_PXN | PTE_UXN;
    } else {
        desc |= PTE_SH_INNER;
        /* exec allowed for now */
    }

    return desc;
}

int mmu_is_enabled(void) {
    return (read_sctlr_el1() & 1ull) != 0;
}

void mmu_init_identity(uint64_t ram_base, uint64_t ram_size) {
    if (mmu_is_enabled()) {
        uart_write("mmu: already enabled\n");
        return;
    }

    uart_write("mmu: building tables\n");

    uint64_t ram_start = align_down(ram_base, 0x200000ull);
    uint64_t ram_end = align_up(ram_base + ram_size, 0x200000ull);
    if (ram_end <= ram_start) {
        uart_write("mmu: bad ram range\n");
        return;
    }

    /* Allocate translation tables */
    uint64_t *l1_low = alloc_table_page();
    uint64_t *l1_high = alloc_table_page();
    uint64_t *l2_0 = alloc_table_page();
    uint64_t *l2_1 = alloc_table_page();
    if (!l1_low || !l1_high || !l2_0 || !l2_1) {
        uart_write("mmu: OOM allocating tables\n");
        return;
    }

    uint64_t l1_low_pa = (uint64_t)(uintptr_t)l1_low;
    uint64_t l1_high_pa = (uint64_t)(uintptr_t)l1_high;
    uint64_t l2_pa = (uint64_t)(uintptr_t)l2_0;
    uint64_t l2_1_pa = (uint64_t)(uintptr_t)l2_1;

    /* TTBR0: L1[0] -> L2 for VA 0..1GB, L1[1] -> L2 for VA 1..2GB */
    l1_low[0] = make_table_desc(l2_pa);
    l1_low[1] = make_table_desc(l2_1_pa);

    /* TTBR1: higher-half base index for 39-bit VA is 256 (VA[38:30] = 0b1_0000_0000). */
    l1_high[256] = make_table_desc(l2_pa);
    l1_high[257] = make_table_desc(l2_1_pa);

    /* Remember the templates used for low identity mapping so we can clone them for per-process TTBR0. */
    g_l2_template0 = l2_0;
    g_l2_template1 = l2_1;

    /* Map 0..1GB in 2MB blocks so we can carve out device range */
    for (uint64_t i = 0; i < TABLE_ENTRIES; i++) {
        uint64_t va = i * 0x200000ull;
        uint64_t pa = va;
        int is_dev = (va >= PERIPH_BASE && va < PERIPH_END);
        int attr = is_dev ? ATTR_DEVICE : ATTR_NORMAL;
        uint64_t ap = PTE_AP_RW_EL1;
        if (!is_dev && va == USER_REGION_BASE) {
            ap = PTE_AP_RW_EL0;
        }
        l2_0[i] = make_block_desc(pa, attr, ap, is_dev);
    }

    /* Map 1GB..2GB in 2MB blocks.
     * This region is mostly unused, but we need the BCM2836/BCM2710 local
     * peripherals at 0x4000_0000.
     */
    for (uint64_t i = 0; i < TABLE_ENTRIES; i++) {
        uint64_t va = 0x40000000ull + i * 0x200000ull;
        uint64_t pa = va;
        int is_dev = (va >= PERIPH_BASE && va < PERIPH_END);
        int attr = is_dev ? ATTR_DEVICE : ATTR_NORMAL;
        l2_1[i] = make_block_desc(pa, attr, PTE_AP_RW_EL1, is_dev);
    }

    /* MAIR: Attr0=Normal WBWA, Attr1=Device-nGnRE */
    uint64_t mair = 0;
    mair |= 0xFFull << 0;  /* normal */
    mair |= 0x04ull << 8;  /* device */
    write_mair_el1(mair);

    /* Install TTBR0/TTBR1 then configure TCR.
     *
     * TCR_EL1 (relevant bits):
     *  - T0SZ [5:0]
     *  - EPD0 [7]
     *  - IRGN0 [9:8]
     *  - ORGN0 [11:10]
     *  - SH0 [13:12]
     *  - TG0 [15:14]
     *  - T1SZ [21:16]
     *  - EPD1 [23]
     *  - IPS [34:32]
     */
    write_ttbr0_el1(l1_low_pa);
    write_ttbr1_el1(l1_high_pa);

    uint64_t tcr = 0;
    tcr |= 25ull;                 /* T0SZ: 39-bit VA */
    tcr |= (25ull << 16);         /* T1SZ: 39-bit VA */

    tcr |= (0ull << 7);           /* EPD0=0: enable TTBR0 walks */
    tcr |= (0ull << 23);          /* EPD1=0: enable TTBR1 walks */

    tcr |= (1ull << 8);           /* IRGN0=01: WBWA */
    tcr |= (1ull << 10);          /* ORGN0=01: WBWA */
    tcr |= (3ull << 12);          /* SH0=3: inner shareable */
    tcr |= (0ull << 14);          /* TG0=0: 4KB */

    tcr |= (1ull << 24);          /* IRGN1=01: WBWA */
    tcr |= (1ull << 26);          /* ORGN1=01: WBWA */
    tcr |= (3ull << 28);          /* SH1=3: inner shareable */
    tcr |= (2ull << 30);          /* TG1=0b10: 4KB */

    tcr |= (2ull << 32);          /* IPS=0b010: 40-bit physical address size */

    write_tcr_el1(tcr);

    uart_write("mmu: enabling\n");

    /* Flush TLBs */
    tlbi_vmalle1();

    /* Enable MMU first (leave caches off until maintenance is done). */
    uint64_t sctlr = read_sctlr_el1();
    sctlr |= (1ull << 0);   /* M */
    sctlr &= ~(1ull << 2);  /* C=0 */
    sctlr &= ~(1ull << 12); /* I=0 */
    write_sctlr_el1(sctlr);

    /* Now that translation is on, perform cache maintenance and enable caches. */
    uart_write("mmu: cache maintenance\n");
    cache_invalidate_all();

    sctlr = read_sctlr_el1();
    sctlr |= (1ull << 2);   /* C */
    sctlr |= (1ull << 12);  /* I */
    write_sctlr_el1(sctlr);

    uart_write("mmu: enabled (identity + higher-half, caches on)\n");
    (void)ram_start;
    (void)ram_end;
}
