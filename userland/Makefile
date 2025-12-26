SHELL := /usr/bin/env bash

CROSS ?= aarch64-elf-
CC := $(CROSS)gcc
LD := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy

BUILD := build

TOOLS_DIR := ../tools
INITRAMFS_DIR := initramfs
INITRAMFS_ROOT := $(BUILD)/initramfs-root

CFLAGS := -O2 -g -ffreestanding -nostdlib -nostartfiles \
	-std=c11 -fno-builtin -fno-stack-protector -fno-pic -fno-plt -fno-asynchronous-unwind-tables \
	-mgeneral-regs-only -mstrict-align \
	-Iinclude -Wall -Wextra -Wno-unused-parameter \
	-Werror=implicit-function-declaration -Werror=implicit-int

# User programs are linked to run at USER_REGION_BASE (identity-mapped in the kernel).
USER_BASE ?= 0x0000000000400000


.PHONY: all clean check-toolchain check-nolibc initramfs

all: check-nolibc $(BUILD)/echo.bin $(BUILD)/true.bin $(BUILD)/cat.bin $(BUILD)/ls.bin $(BUILD)/sh.bin $(BUILD)/init.bin

initramfs: $(BUILD)/initramfs.cpio

check-toolchain:
	@command -v "$(CC)" >/dev/null || { echo "Missing: $(CC)" >&2; exit 2; }
	@command -v "$(LD)" >/dev/null || { echo "Missing: $(LD)" >&2; exit 2; }
	@command -v "$(OBJCOPY)" >/dev/null || { echo "Missing: $(OBJCOPY)" >&2; exit 2; }

check-nolibc:
	@# Prevent accidental libc/glibc dependencies in freestanding userland.
	@bad_headers=$$(grep -RInE --include='*.c' --include='*.h' '^\s*#\s*include\s*<\s*(stdio|stdlib|string|unistd|fcntl|errno|sys/|time|signal|pthread|dlfcn)\.h\s*>' . || true); \
	if [[ -n "$$bad_headers" ]]; then \
		echo "Forbidden libc/glibc headers detected:" >&2; \
		echo "$$bad_headers" >&2; \
		exit 2; \
	fi
	@bad_syms=$$(grep -RInE --include='*.c' --include='*.h' '\\b(printf|fprintf|snprintf|vprintf|vsnprintf|malloc|free|calloc|realloc|strdup|getenv|system|popen)\\b' . || true); \
	if [[ -n "$$bad_syms" ]]; then \
		echo "Forbidden libc-like APIs detected (use raw syscalls / tiny helpers instead):" >&2; \
		echo "$$bad_syms" >&2; \
		exit 2; \
	fi

$(BUILD):
	@mkdir -p "$(BUILD)"

$(BUILD)/crt0.o: src/crt0.S include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/syscall_asm.o: src/syscall_asm.S | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/syscall.o: src/syscall.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/echo.o: src/echo.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/true.o: src/true.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/cat.o: src/cat.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ls.o: src/ls.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/init.o: src/init.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sh.o: src/sh.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/echo.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/echo.o | check-toolchain
	$(LD) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^

$(BUILD)/true.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/true.o | check-toolchain
	$(LD) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^

$(BUILD)/cat.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/cat.o | check-toolchain
	$(LD) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^

$(BUILD)/ls.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/ls.o | check-toolchain
	$(LD) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^

$(BUILD)/init.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/init.o | check-toolchain
	$(LD) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^

$(BUILD)/sh.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/sh.o | check-toolchain
	$(LD) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^

$(BUILD)/echo.bin: $(BUILD)/echo.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/true.bin: $(BUILD)/true.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/cat.bin: $(BUILD)/cat.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/ls.bin: $(BUILD)/ls.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/init.bin: $(BUILD)/init.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/sh.bin: $(BUILD)/sh.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/initramfs.cpio: $(TOOLS_DIR)/mkcpio_newc.py all | $(BUILD)
	@rm -rf "$(INITRAMFS_ROOT)"
	@mkdir -p "$(INITRAMFS_ROOT)/bin"
	@cp -a "$(INITRAMFS_DIR)"/. "$(INITRAMFS_ROOT)"/
	@cp "$(BUILD)/echo.elf" "$(INITRAMFS_ROOT)/bin/echo"
	@cp "$(BUILD)/cat.elf"  "$(INITRAMFS_ROOT)/bin/cat"
	@cp "$(BUILD)/ls.elf"   "$(INITRAMFS_ROOT)/bin/ls"
	@cp "$(BUILD)/sh.elf"   "$(INITRAMFS_ROOT)/bin/sh"
	@cp "$(BUILD)/init.elf" "$(INITRAMFS_ROOT)/init"
	python3 $(TOOLS_DIR)/mkcpio_newc.py --root "$(INITRAMFS_ROOT)" --out $@

clean:
	@rm -rf "$(BUILD)"
