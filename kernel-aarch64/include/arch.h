#pragma once

#include <stdint.h>

static inline uint64_t arch_current_el(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
    return (v >> 2) & 3u;
}
