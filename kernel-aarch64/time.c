#include "time.h"

static uint64_t g_cntfrq_hz = 0;
static uint64_t g_boot_cntpct = 0;
static uint8_t g_time_inited = 0;

static inline uint64_t read_cntfrq_el0(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static inline uint64_t read_cntpct_el0(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

void time_init(void) {
    g_cntfrq_hz = read_cntfrq_el0();
    if (g_cntfrq_hz == 0) {
        /* Should never happen on compliant systems; keep monotonic at 0. */
        g_boot_cntpct = 0;
        g_time_inited = 0;
        return;
    }
    g_boot_cntpct = read_cntpct_el0();
    g_time_inited = 1;
}

uint64_t time_freq_hz(void) {
    return g_cntfrq_hz;
}

uint64_t time_now_ns(void) {
    if (!g_time_inited || g_cntfrq_hz == 0) {
        return 0;
    }

    uint64_t now = read_cntpct_el0();
    uint64_t delta = now - g_boot_cntpct;

    /*
     * ns = (delta / freq) * 1e9 + (delta % freq) * 1e9 / freq
     * Avoid 128-bit division helpers (libgcc) in a freestanding kernel.
     */
    uint64_t freq = g_cntfrq_hz;
    uint64_t sec = delta / freq;
    uint64_t rem = delta - sec * freq;

    uint64_t u64_max = (uint64_t)~0ull;
    if (sec > (u64_max / 1000000000ull)) {
        return u64_max;
    }
    uint64_t ns = sec * 1000000000ull;

    uint64_t frac = (rem * 1000000000ull) / freq;
    if (u64_max - ns < frac) {
        return u64_max;
    }
    return ns + frac;
}
