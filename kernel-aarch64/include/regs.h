#pragma once

#include "stdint.h"

static inline void write_elr_el1(uint64_t v) {
    __asm__ volatile("msr ELR_EL1, %0" :: "r"(v));
}

static inline void write_sp_el0(uint64_t v) {
    __asm__ volatile("msr SP_EL0, %0" :: "r"(v));
}
