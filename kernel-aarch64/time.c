#include "time.h"

static uint64_t g_cntfrq_hz = 0;
static uint64_t g_boot_cntpct = 0;
static uint8_t g_time_inited = 0;
static uint64_t g_tick_interval_cnt = 0;

typedef enum {
    TICK_MODE_DISABLED = 0,
    TICK_MODE_PERIODIC = 1,
    TICK_MODE_ONESHOT = 2,
} tick_mode_t;

static tick_mode_t g_tick_mode = TICK_MODE_DISABLED;

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

static inline void write_cntp_tval_el0(uint64_t v) {
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(v));
}

static inline void write_cntp_ctl_el0(uint64_t v) {
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"(v));
    __asm__ volatile("isb");
}

static inline uint64_t clamp_cntp_tval(uint64_t cnt) {
    /* CNTP_TVAL_EL0 is a signed 32-bit value (ticks until next interrupt). */
    if (cnt == 0) cnt = 1;
    if (cnt > 0x7fffffffull) cnt = 0x7fffffffull;
    return cnt;
}

static uint64_t ns_to_cnt_ticks(uint64_t ns) {
    if (!g_time_inited || g_cntfrq_hz == 0) return 0;

    /* ticks = (ns / 1e9) * freq + (ns % 1e9) * freq / 1e9
     * Avoid 128-bit helpers.
     */
    uint64_t sec = ns / 1000000000ull;
    uint64_t rem = ns - sec * 1000000000ull;

    uint64_t u64_max = (uint64_t)~0ull;
    if (sec != 0 && sec > (u64_max / g_cntfrq_hz)) {
        return u64_max;
    }
    uint64_t ticks = sec * g_cntfrq_hz;

    uint64_t frac = (rem * g_cntfrq_hz) / 1000000000ull;
    if (u64_max - ticks < frac) {
        return u64_max;
    }
    return ticks + frac;
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

void time_tick_init(uint32_t hz) {
    if (!g_time_inited || g_cntfrq_hz == 0) {
        g_tick_interval_cnt = 0;
        g_tick_mode = TICK_MODE_DISABLED;
        return;
    }
    if (hz == 0) hz = 1;

    /* interval = freq / hz (at least 1) */
    uint64_t interval = g_cntfrq_hz / (uint64_t)hz;
    if (interval == 0) interval = 1;

    g_tick_interval_cnt = interval;

    g_tick_mode = TICK_MODE_PERIODIC;

    /* Enable CNTP and unmask its interrupt (IMASK=0). */
    write_cntp_tval_el0(clamp_cntp_tval(g_tick_interval_cnt));
    write_cntp_ctl_el0(1ull);
}

void time_tick_enable_periodic(void) {
    if (!g_time_inited || g_cntfrq_hz == 0) return;
    if (g_tick_interval_cnt == 0) return;
    if (g_tick_mode == TICK_MODE_PERIODIC) return;

    g_tick_mode = TICK_MODE_PERIODIC;
    write_cntp_tval_el0(clamp_cntp_tval(g_tick_interval_cnt));
    write_cntp_ctl_el0(1ull);
}

void time_tick_schedule_oneshot_ns(uint64_t delta_ns) {
    if (!g_time_inited || g_cntfrq_hz == 0) return;

    uint64_t ticks = ns_to_cnt_ticks(delta_ns);
    if (ticks == 0) ticks = 1;

    g_tick_mode = TICK_MODE_ONESHOT;
    write_cntp_tval_el0(clamp_cntp_tval(ticks));
    write_cntp_ctl_el0(1ull);
}

void time_tick_handle_irq(void) {
    if (g_tick_mode == TICK_MODE_PERIODIC) {
        if (g_tick_interval_cnt == 0) return;
        /* Writing TVAL schedules the next event and clears ISTATUS. */
        write_cntp_tval_el0(clamp_cntp_tval(g_tick_interval_cnt));
        return;
    }

    if (g_tick_mode == TICK_MODE_ONESHOT) {
        /* One-shot fired: disable until the scheduler programs the next event. */
        write_cntp_ctl_el0(0ull);
        g_tick_mode = TICK_MODE_DISABLED;
        return;
    }
}
