SHELL := /usr/bin/env bash

CROSS ?= aarch64-elf-
CC := $(CROSS)gcc
LD := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump

BUILD := build

# Track configuration changes (e.g. KERNEL_DEFS) so switching flags triggers the
# minimal required rebuild without needing a full `make clean`.
CONFIG_STAMP := $(BUILD)/.config.stamp

# Which userland binary to embed as the EL0 payload.
USERPROG ?= init

CFLAGS := -O2 -g -ffreestanding -nostdlib -nostartfiles \
	-fno-stack-protector -fno-pic -fno-plt -fno-asynchronous-unwind-tables \
	-mgeneral-regs-only -mstrict-align \
	-Iinclude -I../abi -Wall -Wextra -Wno-unused-parameter

# Optional extra defines/flags (e.g. -DQEMU_SEMIHOSTING)
KERNEL_DEFS ?=
CFLAGS += $(KERNEL_DEFS)

TEST_FAULT ?= 0
CFLAGS += -DTEST_FAULT=$(TEST_FAULT)

LDFLAGS := -T link.ld

OBJS := \
	$(BUILD)/arch/start.o \
	$(BUILD)/arch/exceptions.o \
	$(BUILD)/arch/enter_el0.o \
	$(BUILD)/arch/irq_regtest.o \
	$(BUILD)/main.o \
	$(BUILD)/exceptions.o \
	$(BUILD)/irq.o \
	$(BUILD)/net.o \
	$(BUILD)/net_ipv6.o \
	$(BUILD)/klog.o \
	$(BUILD)/console_in.o \
	$(BUILD)/usb.o \
	$(BUILD)/usb_host.o \
	$(BUILD)/usb_kbd.o \
	$(BUILD)/usb_net.o \
	$(BUILD)/time.o \
	$(BUILD)/power.o \
	$(BUILD)/mailbox.o \
	$(BUILD)/fb.o \
	$(BUILD)/termfb.o \
	$(BUILD)/sys_util.o \
	$(BUILD)/sys_misc.o \
	$(BUILD)/sys_dmesg.o \
	$(BUILD)/sys_net.o \
	$(BUILD)/sys_fs.o \
	$(BUILD)/sys_proc.o \
	$(BUILD)/proc.o \
	$(BUILD)/sched.o \
	$(BUILD)/vfs.o \
	$(BUILD)/pipe.o \
	$(BUILD)/fd.o \
	$(BUILD)/elf64.o \
	$(BUILD)/cpio_newc.o \
	$(BUILD)/initramfs.o \
	$(BUILD)/fdt.o \
	$(BUILD)/cache.o \
	$(BUILD)/mmu.o \
	$(BUILD)/pmm.o \
	$(BUILD)/uart_pl011.o \
	$(BUILD)/user_payload.o \
	$(BUILD)/initramfs_blob.o

.PHONY: all clean disasm check-toolchain FORCE

all: $(BUILD)/kernel8.img

check-toolchain:
	@command -v "$(CC)" >/dev/null || { echo "Missing: $(CC)" >&2; exit 2; }
	@command -v "$(OBJCOPY)" >/dev/null || { echo "Missing: $(OBJCOPY)" >&2; exit 2; }

$(BUILD):
	@mkdir -p $(BUILD)/arch

$(CONFIG_STAMP): FORCE | $(BUILD)
	@tmp="$@.tmp"; \
	printf '%s\n' "CFLAGS=$(CFLAGS)" >"$$tmp"; \
	if [[ ! -f "$@" ]] || ! cmp -s "$$tmp" "$@"; then \
		mv "$$tmp" "$@"; \
	else \
		rm -f "$$tmp"; \
	fi

$(BUILD)/arch/start.o: arch/start.S $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/arch/exceptions.o: arch/exceptions.S $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/arch/enter_el0.o: arch/enter_el0.S $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/arch/irq_regtest.o: arch/irq_regtest.S $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/main.o: main.c include/uart_pl011.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/console_in.o: console_in.c include/console_in.h include/time.h include/uart_pl011.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/klog.o: klog.c include/klog.h include/stdint.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@


$(BUILD)/usb.o: usb.c include/usb.h include/usb_host.h include/usb_kbd.h include/usb_net.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/usb_host.o: usb_host.c include/usb_host.h include/stddef.h include/stdint.h include/mmu.h include/time.h include/uart_pl011.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/usb_kbd.o: usb_kbd.c include/usb_kbd.h include/usb_host.h include/console_in.h include/uart_pl011.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/usb_net.o: usb_net.c include/usb_net.h include/usb_host.h include/net.h include/uart_pl011.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/uart_pl011.o: uart_pl011.c include/uart_pl011.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@


$(BUILD)/user_payload.bin: ../userland/build/$(USERPROG).bin FORCE | $(BUILD)
	@cp $< $@

FORCE:

$(BUILD)/initramfs_blob.o: initramfs_blob.S ../userland/build/initramfs.cpio $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@


$(BUILD)/user_payload.o: user_payload.S $(BUILD)/user_payload.bin $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/cpio_newc.o: cpio_newc.c include/cpio_newc.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/initramfs.o: initramfs.c include/initramfs.h include/cpio_newc.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@


$(BUILD)/exceptions.o: exceptions.c include/exceptions.h include/errno.h include/syscalls.h include/proc.h include/sched.h include/uart_pl011.h include/irq.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/irq.o: irq.c include/irq.h include/time.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/net.o: net.c include/net.h include/net_ipv6.h include/stddef.h include/stdint.h include/uart_pl011.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/net_ipv6.o: net_ipv6.c include/net_ipv6.h include/net.h include/proc.h include/time.h include/errno.h include/uart_pl011.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/time.o: time.c include/time.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/power.o: power.c include/power.h include/errno.h include/uart_pl011.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/mailbox.o: mailbox.c include/mailbox.h include/stddef.h include/stdint.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/fb.o: fb.c include/fb.h include/mailbox.h include/mmu.h include/pmm.h include/uart_pl011.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/termfb.o: termfb.c include/termfb.h include/fb.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sys_util.o: sys_util.c include/sys_util.h include/proc.h include/mmu.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sys_misc.o: sys_misc.c include/syscalls.h include/sys_util.h include/errno.h include/linux_abi.h include/power.h include/proc.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sys_dmesg.o: sys_dmesg.c include/syscalls.h include/sys_util.h include/errno.h include/klog.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sys_net.o: sys_net.c include/syscalls.h include/sys_util.h include/errno.h include/net.h include/net_ipv6.h include/proc.h include/sched.h include/time.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sys_fs.o: sys_fs.c include/syscalls.h include/sys_util.h include/errno.h include/linux_abi.h include/fd.h include/pipe.h include/vfs.h include/initramfs.h include/proc.h include/uart_pl011.h include/console_in.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sys_proc.o: sys_proc.c include/syscalls.h include/sys_util.h include/errno.h include/linux_abi.h include/proc.h include/regs.h include/sched.h include/mmu.h include/pmm.h include/elf64.h include/cache.h include/initramfs.h include/power.h include/uart_pl011.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/proc.o: proc.c include/proc.h include/fd.h include/pipe.h include/vfs.h include/mmu.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sched.o: sched.c include/sched.h include/proc.h include/regs.h include/mmu.h include/sys_util.h include/console_in.h include/irq.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/vfs.o: vfs.c include/vfs.h include/initramfs.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/pipe.o: pipe.c include/pipe.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/fd.o: fd.c include/fd.h include/pipe.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/elf64.o: elf64.c include/elf64.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/fdt.o: fdt.c include/fdt.h include/uart_pl011.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/pmm.o: pmm.c include/pmm.h include/uart_pl011.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/mmu.o: mmu.c include/mmu.h include/pmm.h include/uart_pl011.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/cache.o: cache.c include/cache.h $(CONFIG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel.elf: $(OBJS) link.ld $(CONFIG_STAMP) | check-toolchain
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD)/kernel8.img: $(BUILD)/kernel.elf
	$(OBJCOPY) -O binary $< $@
	@echo "OK: built $@"

disasm: $(BUILD)/kernel.elf
	$(OBJDUMP) -d $< | less

clean:
	@rm -rf $(BUILD)
