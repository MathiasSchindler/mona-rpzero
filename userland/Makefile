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
	-Iinclude -I../abi -Wall -Wextra -Wno-unused-parameter \
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

all: check-nolibc $(BUILD)/echo.bin $(BUILD)/true.bin $(BUILD)/false.bin $(BUILD)/cat.bin $(BUILD)/ls.bin $(BUILD)/pwd.bin $(BUILD)/pid.bin $(BUILD)/uname.bin $(BUILD)/mkdir.bin $(BUILD)/touch.bin $(BUILD)/rm.bin $(BUILD)/rmdir.bin $(BUILD)/seq.bin $(BUILD)/uniq.bin $(BUILD)/wc.bin $(BUILD)/grep.bin $(BUILD)/ps.bin $(BUILD)/kill.bin $(BUILD)/pstree.bin $(BUILD)/find.bin $(BUILD)/awk.bin $(BUILD)/basename.bin $(BUILD)/du.bin $(BUILD)/ln.bin $(BUILD)/tr.bin $(BUILD)/sed.bin $(BUILD)/head.bin $(BUILD)/tail.bin $(BUILD)/sort.bin $(BUILD)/time.bin $(BUILD)/dmesg.bin $(BUILD)/readelf.bin $(BUILD)/readlink.bin $(BUILD)/brk.bin $(BUILD)/mmap.bin $(BUILD)/cwd.bin $(BUILD)/tty.bin $(BUILD)/sleep.bin $(BUILD)/date.bin $(BUILD)/uptime.bin $(BUILD)/compat.bin $(BUILD)/kinit.bin $(BUILD)/sh.bin $(BUILD)/init.bin

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

$(BUILD)/false.o: src/false.c include/syscall.h | $(BUILD)
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

$(BUILD)/touch.o: src/touch.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/rm.o: src/rm.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/rmdir.o: src/rmdir.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/seq.o: src/seq.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/uniq.o: src/uniq.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/wc.o: src/wc.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/grep.o: src/grep.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/awk.o: src/awk.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/basename.o: src/basename.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/du.o: src/du.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ln.o: src/ln.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/tr.o: src/tr.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sed.o: src/sed.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/head.o: src/head.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/tail.o: src/tail.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sort.o: src/sort.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/time.o: src/time.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/dmesg.o: src/dmesg.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/readelf.o: src/readelf.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/readlink.o: src/readlink.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ps.o: src/ps.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kill.o: src/kill.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/pstree.o: src/pstree.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/find.o: src/find.c include/syscall.h | $(BUILD)
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

$(BUILD)/date.o: src/date.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/uptime.o: src/uptime.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/compat.o: src/compat.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/init.o: src/init.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kinit.o: src/kinit.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sh.o: src/sh.c include/syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/echo.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/echo.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/true.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/true.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/false.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/false.o | check-toolchain
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

$(BUILD)/touch.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/touch.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/rm.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/rm.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/rmdir.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/rmdir.o | check-toolchain
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

$(BUILD)/grep.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/grep.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/awk.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/awk.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/basename.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/basename.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/du.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/du.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/ln.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/ln.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/tr.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/tr.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/sed.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/sed.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/head.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/head.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/tail.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/tail.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/sort.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/sort.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/time.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/time.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/dmesg.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/dmesg.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/readelf.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/readelf.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/readlink.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/readlink.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/ps.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/ps.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/kill.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/kill.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/pstree.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/pstree.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/find.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/find.o | check-toolchain
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

$(BUILD)/date.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/date.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/uptime.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/uptime.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/compat.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/compat.o | check-toolchain
	$(LD) $(LDFLAGS) -e _start -T link.ld -defsym=__user_base=$(USER_BASE) -o $@ $^
	$(POSTLINK)

$(BUILD)/kinit.elf: $(BUILD)/crt0.o $(BUILD)/syscall_asm.o $(BUILD)/syscall.o $(BUILD)/kinit.o | check-toolchain
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

$(BUILD)/false.bin: $(BUILD)/false.elf | check-toolchain
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

$(BUILD)/touch.bin: $(BUILD)/touch.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/rm.bin: $(BUILD)/rm.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/rmdir.bin: $(BUILD)/rmdir.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/seq.bin: $(BUILD)/seq.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/uniq.bin: $(BUILD)/uniq.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/wc.bin: $(BUILD)/wc.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/grep.bin: $(BUILD)/grep.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/awk.bin: $(BUILD)/awk.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/basename.bin: $(BUILD)/basename.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/du.bin: $(BUILD)/du.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/ln.bin: $(BUILD)/ln.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/tr.bin: $(BUILD)/tr.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/sed.bin: $(BUILD)/sed.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/head.bin: $(BUILD)/head.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/tail.bin: $(BUILD)/tail.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/sort.bin: $(BUILD)/sort.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/time.bin: $(BUILD)/time.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/dmesg.bin: $(BUILD)/dmesg.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/readelf.bin: $(BUILD)/readelf.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/readlink.bin: $(BUILD)/readlink.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/ps.bin: $(BUILD)/ps.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/kill.bin: $(BUILD)/kill.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/pstree.bin: $(BUILD)/pstree.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/find.bin: $(BUILD)/find.elf | check-toolchain
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

$(BUILD)/date.bin: $(BUILD)/date.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/uptime.bin: $(BUILD)/uptime.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/compat.bin: $(BUILD)/compat.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/kinit.bin: $(BUILD)/kinit.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/init.bin: $(BUILD)/init.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/sh.bin: $(BUILD)/sh.elf | check-toolchain
	$(OBJCOPY) -O binary $< $@

$(BUILD)/initramfs.cpio: $(TOOLS_DIR)/mkcpio_newc.py all | $(BUILD)
	@rm -rf "$(INITRAMFS_ROOT)"
	@mkdir -p "$(INITRAMFS_ROOT)/bin"
	@cp -a "$(INITRAMFS_DIR)"/. "$(INITRAMFS_ROOT)"/
	@# Drop host OS metadata files that may exist on macOS.
	@/usr/bin/find "$(INITRAMFS_ROOT)" \( -name '.DS_Store' -o -name '._*' \) -type f -delete 2>/dev/null || true
	@cp "$(BUILD)/echo.elf" "$(INITRAMFS_ROOT)/bin/echo"
	@cp "$(BUILD)/true.elf" "$(INITRAMFS_ROOT)/bin/true"
	@cp "$(BUILD)/false.elf" "$(INITRAMFS_ROOT)/bin/false"
	@cp "$(BUILD)/cat.elf"  "$(INITRAMFS_ROOT)/bin/cat"
	@cp "$(BUILD)/ls.elf"   "$(INITRAMFS_ROOT)/bin/ls"
	@cp "$(BUILD)/pwd.elf"  "$(INITRAMFS_ROOT)/bin/pwd"
	@cp "$(BUILD)/pid.elf"  "$(INITRAMFS_ROOT)/bin/pid"
	@cp "$(BUILD)/uname.elf"  "$(INITRAMFS_ROOT)/bin/uname"
	@cp "$(BUILD)/mkdir.elf"  "$(INITRAMFS_ROOT)/bin/mkdir"
	@cp "$(BUILD)/touch.elf"  "$(INITRAMFS_ROOT)/bin/touch"
	@cp "$(BUILD)/rm.elf"  "$(INITRAMFS_ROOT)/bin/rm"
	@cp "$(BUILD)/rmdir.elf"  "$(INITRAMFS_ROOT)/bin/rmdir"
	@cp "$(BUILD)/seq.elf"  "$(INITRAMFS_ROOT)/bin/seq"
	@cp "$(BUILD)/uniq.elf"  "$(INITRAMFS_ROOT)/bin/uniq"
	@cp "$(BUILD)/wc.elf"  "$(INITRAMFS_ROOT)/bin/wc"
	@cp "$(BUILD)/grep.elf"  "$(INITRAMFS_ROOT)/bin/grep"
	@cp "$(BUILD)/awk.elf"  "$(INITRAMFS_ROOT)/bin/awk"
	@cp "$(BUILD)/basename.elf"  "$(INITRAMFS_ROOT)/bin/basename"
	@cp "$(BUILD)/du.elf"  "$(INITRAMFS_ROOT)/bin/du"
	@cp "$(BUILD)/ln.elf"  "$(INITRAMFS_ROOT)/bin/ln"
	@cp "$(BUILD)/tr.elf"  "$(INITRAMFS_ROOT)/bin/tr"
	@cp "$(BUILD)/sed.elf"  "$(INITRAMFS_ROOT)/bin/sed"
	@cp "$(BUILD)/head.elf"  "$(INITRAMFS_ROOT)/bin/head"
	@cp "$(BUILD)/tail.elf"  "$(INITRAMFS_ROOT)/bin/tail"
	@cp "$(BUILD)/sort.elf"  "$(INITRAMFS_ROOT)/bin/sort"
	@cp "$(BUILD)/time.elf"  "$(INITRAMFS_ROOT)/bin/time"
	@cp "$(BUILD)/dmesg.elf"  "$(INITRAMFS_ROOT)/bin/dmesg"
	@cp "$(BUILD)/readelf.elf"  "$(INITRAMFS_ROOT)/bin/readelf"
	@cp "$(BUILD)/readlink.elf"  "$(INITRAMFS_ROOT)/bin/readlink"
	@cp "$(BUILD)/ps.elf"  "$(INITRAMFS_ROOT)/bin/ps"
	@cp "$(BUILD)/kill.elf"  "$(INITRAMFS_ROOT)/bin/kill"
	@cp "$(BUILD)/pstree.elf"  "$(INITRAMFS_ROOT)/bin/pstree"
	@cp "$(BUILD)/find.elf"  "$(INITRAMFS_ROOT)/bin/find"
	@cp "$(BUILD)/brk.elf"  "$(INITRAMFS_ROOT)/bin/brk"
	@cp "$(BUILD)/mmap.elf"  "$(INITRAMFS_ROOT)/bin/mmap"
	@cp "$(BUILD)/cwd.elf"  "$(INITRAMFS_ROOT)/bin/cwd"
	@cp "$(BUILD)/tty.elf"  "$(INITRAMFS_ROOT)/bin/tty"
	@cp "$(BUILD)/sleep.elf"  "$(INITRAMFS_ROOT)/bin/sleep"
	@cp "$(BUILD)/date.elf"  "$(INITRAMFS_ROOT)/bin/date"
	@cp "$(BUILD)/uptime.elf"  "$(INITRAMFS_ROOT)/bin/uptime"
	@cp "$(BUILD)/compat.elf"  "$(INITRAMFS_ROOT)/bin/compat"
	@cp "$(BUILD)/kinit.elf"  "$(INITRAMFS_ROOT)/bin/kinit"
	@cp "$(BUILD)/sh.elf"   "$(INITRAMFS_ROOT)/bin/sh"
	@cp "$(BUILD)/init.elf" "$(INITRAMFS_ROOT)/init"
	python3 $(TOOLS_DIR)/mkcpio_newc.py --root "$(INITRAMFS_ROOT)" --out $@

clean:
	@rm -rf "$(BUILD)"
