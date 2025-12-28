#pragma once

#include "stdint.h"

__attribute__((noreturn)) void kernel_poweroff(void);

/* Linux reboot syscall (minimal subset). */
uint64_t sys_reboot(uint64_t magic1, uint64_t magic2, uint64_t cmd, uint64_t arg);
