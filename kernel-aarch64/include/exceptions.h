#pragma once

#include <stdint.h>

typedef struct trap_frame {
    uint64_t x[31];
    uint64_t sp_el0;
} trap_frame_t;

uint64_t exception_handle(trap_frame_t *tf,
                          uint64_t kind,
                          uint64_t esr,
                          uint64_t elr,
                          uint64_t far,
                          uint64_t spsr);
