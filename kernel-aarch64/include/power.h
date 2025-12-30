#pragma once

#include "stdint.h"

__attribute__((noreturn)) void kernel_poweroff(void);

__attribute__((noreturn)) void kernel_poweroff_with_code(uint32_t code);
