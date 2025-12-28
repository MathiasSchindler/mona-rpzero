SHELL := /usr/bin/env bash

CROSS ?= aarch64-elf-
CC := $(CROSS)gcc
LD := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump

BUILD := build

# Which userland binary to embed as the EL0 payload.
USERPROG ?= init

CFLAGS := -O2 -g -ffreestanding -nostdlib -nostartfiles \
	-fno-stack-protector -fno-pic -fno-plt -fno-asynchronous-unwind-tables \
	-mgeneral-regs-only -mstrict-align \
	-Iinclude -Wall -Wextra -Wno-unused-parameter

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
	$(BUILD)/main.o \
	$(BUILD)/exceptions.o \
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

$(BUILD)/arch/start.o: arch/start.S | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/arch/exceptions.o: arch/exceptions.S | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/arch/enter_el0.o: arch/enter_el0.S | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/main.o: main.c include/uart_pl011.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/uart_pl011.o: uart_pl011.c include/uart_pl011.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@


$(BUILD)/user_payload.bin: ../userland/build/$(USERPROG).bin FORCE | $(BUILD)
	@cp $< $@

FORCE:

$(BUILD)/initramfs_blob.o: initramfs_blob.S ../userland/build/initramfs.cpio | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@


$(BUILD)/user_payload.o: user_payload.S $(BUILD)/user_payload.bin | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/cpio_newc.o: cpio_newc.c include/cpio_newc.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/initramfs.o: initramfs.c include/initramfs.h include/cpio_newc.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/exceptions.o: exceptions.c include/uart_pl011.h include/exceptions.h include/initramfs.h include/mmu.h include/linux_abi.h include/elf64.h include/cache.h include/vfs.h include/pipe.h include/fd.h include/proc.h include/sched.h include/regs.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/proc.o: proc.c include/proc.h include/fd.h include/pipe.h include/vfs.h include/mmu.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sched.o: sched.c include/sched.h include/proc.h include/regs.h include/mmu.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/vfs.o: vfs.c include/vfs.h include/initramfs.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/pipe.o: pipe.c include/pipe.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/fd.o: fd.c include/fd.h include/pipe.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/elf64.o: elf64.c include/elf64.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/fdt.o: fdt.c include/fdt.h include/uart_pl011.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/pmm.o: pmm.c include/pmm.h include/uart_pl011.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/mmu.o: mmu.c include/mmu.h include/pmm.h include/uart_pl011.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/cache.o: cache.c include/cache.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel.elf: $(OBJS) link.ld | check-toolchain
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD)/kernel8.img: $(BUILD)/kernel.elf
	$(OBJCOPY) -O binary $< $@
	@echo "OK: built $@"

disasm: $(BUILD)/kernel.elf
	$(OBJDUMP) -d $< | less

clean:
	@rm -rf $(BUILD)
