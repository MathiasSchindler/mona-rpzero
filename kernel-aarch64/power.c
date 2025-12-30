#include "power.h"

#include "errno.h"
#include "uart_pl011.h"

/* PSCI 0.2+ system off (SMC) */
#define PSCI_FN_SYSTEM_OFF 0x84000008ull

__attribute__((noreturn)) void kernel_poweroff_with_code(uint32_t code) {
#ifdef QEMU_SEMIHOSTING
    /* QEMU semihosting: request the emulator to exit.
     * Requires QEMU to be started with -semihosting.
     */
    uart_write("[kernel] poweroff: semihosting SYS_EXIT_EXTENDED\n");
    struct {
        uint32_t reason;
        uint32_t subcode;
    } args = {0x20026u, code}; /* ADP_Stopped_ApplicationExit */

    register uint64_t x0 __asm__("x0") = 0x20ull; /* SYS_EXIT_EXTENDED */
    register void *x1 __asm__("x1") = &args;
    __asm__ volatile(
        "hlt #0xf000\n"
        :
        : "r"(x0), "r"(x1)
        : "memory");
#endif

    uart_write("[kernel] poweroff: PSCI SYSTEM_OFF\n");
    register uint64_t x0_psci __asm__("x0") = PSCI_FN_SYSTEM_OFF;
    __asm__ volatile(
        "smc #0\n"
        : "+r"(x0_psci)
        :
        : "x1", "x2", "x3", "x4", "x5", "x6", "x7", "memory");

    /* If we get here, firmware/QEMU didn't honor SYSTEM_OFF. */
    uart_write("[kernel] poweroff: PSCI returned, halting (wfe loop)\n");
    for (;;) {
        __asm__ volatile("wfe");
    }
}

__attribute__((noreturn)) void kernel_poweroff(void) {
    kernel_poweroff_with_code(0);
}

uint64_t sys_reboot(uint64_t magic1, uint64_t magic2, uint64_t cmd, uint64_t arg) {
    (void)magic1;
    (void)magic2;
    (void)arg;

    /* Minimal support: power off for now. */
    const uint64_t LINUX_REBOOT_CMD_POWER_OFF = 0x4321fedcull;
    if (cmd == LINUX_REBOOT_CMD_POWER_OFF) {
        kernel_poweroff_with_code((uint32_t)(arg & 0xffu));
    }

    return (uint64_t)(-(int64_t)EINVAL);
}
