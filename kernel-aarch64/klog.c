#include "klog.h"

/* Keep this modest; can be tuned later. */
enum { KLOG_CAP = 64 * 1024 };

static char g_klog[KLOG_CAP];
static uint64_t g_head = 0;  /* next write position */
static uint64_t g_len = 0;   /* number of valid bytes */

void klog_putc(char c) {
    g_klog[g_head] = c;
    g_head = (g_head + 1u) % (uint64_t)KLOG_CAP;
    if (g_len < (uint64_t)KLOG_CAP) {
        g_len++;
    }
}

uint64_t klog_len(void) {
    return g_len;
}

char klog_at(uint64_t i) {
    if (g_len == 0) return 0;
    if (i >= g_len) i = g_len - 1u;

    uint64_t start = (g_head + (uint64_t)KLOG_CAP - g_len) % (uint64_t)KLOG_CAP;
    uint64_t idx = (start + i) % (uint64_t)KLOG_CAP;
    return g_klog[idx];
}

void klog_clear(void) {
    g_head = 0;
    g_len = 0;
}
