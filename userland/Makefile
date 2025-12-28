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
	-ffunction-sections -fdata-sections \
	-mgeneral-regs-only -mstrict-align \
	-Iinclude -Wall -Wextra -Wno-unused-parameter \
	-Werror=implicit-function-declaration -Werror=implicit-int

LDFLAGS := --gc-sections -z max-page-size=4096 -z common-page-size=4096

# Keep full debug info in separate files while shipping stripped ELFs in initramfs.
# GDB can pick up symbols via the embedded .gnu_debuglink (files live next to the build artifacts).
SPLIT_DEBUG ?= 1

define POSTLINK
	@if [[ "$(SPLIT_DEBUG)" == "1" ]]; then \
		$(OBJCOPY) --only-keep-debug $@ $@.debug; \
		$(OBJCOPY) --strip-all $@; \
		$(OBJCOPY) --add-gnu-debuglink=$@.debug $@; \
	fi
endef

# User programs are linked to run at USER_REGION_BASE (identity-mapped in the kernel).
USER_BASE ?= 0x0000000000400000





.PHONY: all clean check-toolchain check-nolibc initramfs

all: check-nolibc $(BUILD)/echo.bin $(BUILD)/true.bin $(BUILD)/cat.bin $(BUILD)/ls.bin $(BUILD)/pwd.bin $(BUILD)/pid.bin $(BUILD)/uname.bin $(BUILD)/mkdir.bin $(BUILD)/seq.bin $(BUILD)/uniq.bin $(BUILD)/wc.bin $(BUILD)/brk.bin $(BUILD)/mmap.bin $(BUILD)/cwd.bin $(BUILD)/tty.bin $(BUILD)/sleep.bin $(BUILD)/compat.bin $(BUILD)/sh.bin $(BUILD)/init.bin

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

$(BUILD)/pwd.o: src/pwd.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/pid.o: src/pid.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/uname.o: src/uname.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/mkdir.o: src/mkdir.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/seq.o: src/seq.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/uniq.o: src/uniq.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/wc.o: src/wc.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/brk.o: src/brk.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/mmap.o: src/mmap.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/cwd.o: src/cwd.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/tty.o: src/tty.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sleep.o: src/sleep.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/compat.o: src/compat.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/init.o: src/init.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sh.o: src/sh.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/echo.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/echo.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/true.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/true.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/cat.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/cat.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/ls.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/ls.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/pwd.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/pwd.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/pid.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/pid.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/uname.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/uname.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/mkdir.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/mkdir.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/seq.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/seq.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/uniq.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/uniq.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/wc.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/wc.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/brk.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/brk.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/mmap.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/mmap.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/cwd.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/cwd.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/tty.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/tty.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/sleep.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/sleep.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/compat.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/compat.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/init.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/init.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/sh.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/sh.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/echo.bin: $(BUILD)/echo.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/true.bin: $(BUILD)/true.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/cat.bin: $(BUILD)/cat.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/ls.bin: $(BUILD)/ls.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/pwd.bin: $(BUILD)/pwd.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/pid.bin: $(BUILD)/pid.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/uname.bin: $(BUILD)/uname.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/mkdir.bin: $(BUILD)/mkdir.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/seq.bin: $(BUILD)/seq.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/uniq.bin: $(BUILD)/uniq.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/wc.bin: $(BUILD)/wc.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/brk.bin: $(BUILD)/brk.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/mmap.bin: $(BUILD)/mmap.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/cwd.bin: $(BUILD)/cwd.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/tty.bin: $(BUILD)/tty.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/sleep.bin: $(BUILD)/sleep.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/compat.bin: $(BUILD)/compat.elf | check-toolchain
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
	@cp "$(BUILD)/pwd.elf"  "$(INITRAMFS_ROOT)/bin/pwd"
	@cp "$(BUILD)/pid.elf"  "$(INITRAMFS_ROOT)/bin/pid"
	@cp "$(BUILD)/uname.elf"  "$(INITRAMFS_ROOT)/bin/uname"
	@cp "$(BUILD)/mkdir.elf"  "$(INITRAMFS_ROOT)/bin/mkdir"
	@cp "$(BUILD)/seq.elf"  "$(INITRAMFS_ROOT)/bin/seq"
	@cp "$(BUILD)/uniq.elf"  "$(INITRAMFS_ROOT)/bin/uniq"
	@cp "$(BUILD)/wc.elf"  "$(INITRAMFS_ROOT)/bin/wc"
	@cp "$(BUILD)/brk.elf"  "$(INITRAMFS_ROOT)/bin/brk"
	@cp "$(BUILD)/mmap.elf"  "$(INITRAMFS_ROOT)/bin/mmap"
	@cp "$(BUILD)/cwd.elf"  "$(INITRAMFS_ROOT)/bin/cwd"
	@cp "$(BUILD)/tty.elf"  "$(INITRAMFS_ROOT)/bin/tty"
	@cp "$(BUILD)/sleep.elf"  "$(INITRAMFS_ROOT)/bin/sleep"
	@cp "$(BUILD)/compat.elf"  "$(INITRAMFS_ROOT)/bin/compat"
	@cp "$(BUILD)/sh.elf"   "$(INITRAMFS_ROOT)/bin/sh"
	@cp "$(BUILD)/init.elf" "$(INITRAMFS_ROOT)/init"
	python3 $(TOOLS_DIR)/mkcpio_newc.py --root "$(INITRAMFS_ROOT)" --out $@

clean:
	@rm -rf "$(BUILD)"
