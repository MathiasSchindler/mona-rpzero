#include "cache.h"

static inline uint64_t read_clidr_el1(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, clidr_el1" : "=r"(v));
    return v;
}

static inline uint64_t read_ccsidr_el1(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, ccsidr_el1" : "=r"(v));
    return v;
}

static inline void write_csselr_el1(uint64_t v) {
    __asm__ volatile("msr csselr_el1, %0" :: "r"(v));
    __asm__ volatile("isb");
}

static inline void dsb_ish(void) { __asm__ volatile("dsb ish" ::: "memory"); }
static inline void dsb_sy(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static inline void isb(void) { __asm__ volatile("isb" ::: "memory"); }

static inline void ic_iallu(void) {
    __asm__ volatile("ic iallu" ::: "memory");
}

static inline void ic_ivau(uint64_t v) {
    __asm__ volatile("ic ivau, %0" :: "r"(v) : "memory");
}

static inline void dc_cvau(uint64_t v) {
    __asm__ volatile("dc cvau, %0" :: "r"(v) : "memory");
}

static inline uint64_t read_ctr_el0(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(v));
    return v;
}

static inline void dc_isw(uint64_t v) {
    __asm__ volatile("dc isw, %0" :: "r"(v) : "memory");
}

static inline void dc_cisw(uint64_t v) {
    __asm__ volatile("dc cisw, %0" :: "r"(v) : "memory");
}

static inline unsigned ulog2_floor_u32(uint32_t v) {
    return 31u - (unsigned)__builtin_clz(v);
}

void cache_invalidate_all(void) {
    /* Invalidate instruction cache to PoU */
    dsb_ish();
    ic_iallu();
    dsb_ish();
    isb();

    /* Invalidate data/unified caches by set/way */
    uint64_t clidr = read_clidr_el1();

    for (unsigned level = 0; level < 7; level++) {
        unsigned ctype = (unsigned)((clidr >> (level * 3)) & 0x7);
        if (ctype == 0) {
            continue; /* no cache at this level */
        }

        /* 2 = instruction only; skip. 1=data, 3=separate, 4=unified */
        if (ctype == 2) {
            continue;
        }

        /* Select data/unified cache at this level: InD=0 */
        write_csselr_el1((uint64_t)(level << 1));
        uint64_t ccsidr = read_ccsidr_el1();

        /*
         * CCSIDR:
         *  - LineSize [2:0] (log2(#words) - 2). For bytes: +4 gives log2(bytes).
         *  - NumWays  [12:3] (ways-1)
         *  - NumSets  [27:13] (sets-1)
         */
        unsigned line_len = (unsigned)((ccsidr & 0x7) + 4); /* log2(bytes per line) */
        unsigned num_ways = (unsigned)(((ccsidr >> 3) & 0x3FF) + 1);
        unsigned num_sets = (unsigned)(((ccsidr >> 13) & 0x7FFF) + 1);

        unsigned way_bits = (num_ways > 1) ? (ulog2_floor_u32((uint32_t)num_ways - 1) + 1) : 0;
        unsigned way_shift = 32u - way_bits;

        for (unsigned way = 0; way < num_ways; way++) {
            for (unsigned set = 0; set < num_sets; set++) {
                uint64_t sw = 0;
                sw |= (uint64_t)(level << 1);
                sw |= (uint64_t)set << line_len;
                if (way_bits != 0) {
                    sw |= (uint64_t)way << way_shift;
                }
                dc_isw(sw);
            }
        }
    }

    dsb_sy();
    isb();
}

void cache_clean_invalidate_all(void) {
    /* Clean+invalidate data/unified caches by set/way.
     * This is heavy but safe: it avoids losing dirty lines and prevents stale
     * VA-tagged cache lines from leaking across address space switches.
     */
    uint64_t clidr = read_clidr_el1();

    for (unsigned level = 0; level < 7; level++) {
        unsigned ctype = (unsigned)((clidr >> (level * 3)) & 0x7);
        if (ctype == 0) {
            continue; /* no cache at this level */
        }
        /* 2 = instruction only; skip. 1=data, 3=separate, 4=unified */
        if (ctype == 2) {
            continue;
        }

        /* Select data/unified cache at this level: InD=0 */
        write_csselr_el1((uint64_t)(level << 1));
        uint64_t ccsidr = read_ccsidr_el1();

        unsigned line_len = (unsigned)((ccsidr & 0x7) + 4); /* log2(bytes per line) */
        unsigned num_ways = (unsigned)(((ccsidr >> 3) & 0x3FF) + 1);
        unsigned num_sets = (unsigned)(((ccsidr >> 13) & 0x7FFF) + 1);

        unsigned way_bits = (num_ways > 1) ? (ulog2_floor_u32((uint32_t)num_ways - 1) + 1) : 0;
        unsigned way_shift = 32u - way_bits;

        for (unsigned way = 0; way < num_ways; way++) {
            for (unsigned set = 0; set < num_sets; set++) {
                uint64_t sw = 0;
                sw |= (uint64_t)(level << 1);
                sw |= (uint64_t)set << line_len;
                if (way_bits != 0) {
                    sw |= (uint64_t)way << way_shift;
                }
                dc_cisw(sw);
            }
        }
    }

    dsb_sy();

    /* Invalidate instruction cache to PoU */
    dsb_ish();
    ic_iallu();
    dsb_ish();
    isb();
}

void cache_sync_icache_for_range(uint64_t start, uint64_t size) {
    if (size == 0) {
        return;
    }

    /* CTR_EL0.DminLine (bits [19:16]) gives log2(words) => bytes = 4 << n. */
    uint64_t ctr = read_ctr_el0();
    uint64_t dminline_words_log2 = (ctr >> 16) & 0xF;
    uint64_t line = 4ull << dminline_words_log2;
    if (line < 16) line = 16;
    if (line > 256) line = 256;

    uint64_t s = start & ~(line - 1);
    uint64_t e = (start + size + (line - 1)) & ~(line - 1);

    for (uint64_t p = s; p < e; p += line) {
        dc_cvau(p);
    }
    dsb_ish();

    for (uint64_t p = s; p < e; p += line) {
        ic_ivau(p);
    }
    dsb_ish();
    isb();
}
