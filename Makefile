# Minimal workflow (bare-metal kernel + userland) for QEMU raspi3b (Pi Zero 2 W-ish)
#
# Targets:
#   make clean  - remove all build artifacts
#   make        - build bare-metal kernel + userland
#   make run    - boot bare-metal kernel in QEMU

SHELL := /usr/bin/env bash

# Device Tree Blob (DTB): describes the board's hardware (RAM, UART, MMIO ranges, interrupts).
# QEMU can pass it to the kernel via `-dtb`, and on real Pi firmware passes a DTB pointer.
#
# By default we auto-detect a DTB in common locations. You can always override:
#   make run DTB=path/to/some.dtb
DTB_CANDIDATES := \
	out/bcm2710-rpi-3-b.dtb \
	out/bcm2710-rpi-zero-2-w.dtb \
	archive/bcm2710-rpi-3-b.dtb \
	archive/bcm2710-rpi-zero-2-w.dtb

DTB ?= $(firstword $(wildcard $(DTB_CANDIDATES)))

# QEMU raspi3b currently requires exactly 1 GiB RAM.
MEM ?= 1024

# Optional: which userland tool to embed as the EL0 payload.
USERPROG ?= init


.PHONY: all run clean
.PHONY: aarch64-kernel run-aarch64 userland

all: aarch64-kernel

# --- Bare-metal AArch64 kernel (Phase 0) ---

AARCH64_DIR ?= kernel-aarch64
# Homebrew on macOS typically provides the bare-metal toolchain as `aarch64-elf-*`.
# You can override this (e.g. `make AARCH64_CROSS=aarch64-linux-gnu- ...`).
AARCH64_CROSS ?= aarch64-elf-
AARCH64_IMG := $(AARCH64_DIR)/build/kernel8.img

USERLAND_DIR ?= userland

userland:
	@$(MAKE) -C "$(USERLAND_DIR)" CROSS="$(AARCH64_CROSS)" all
	@$(MAKE) -C "$(USERLAND_DIR)" CROSS="$(AARCH64_CROSS)" initramfs


aarch64-kernel: userland
	@$(MAKE) -C "$(AARCH64_DIR)" CROSS="$(AARCH64_CROSS)" USERPROG="$(USERPROG)" all

run-aarch64: aarch64-kernel
	@echo "Starting QEMU (raspi3b) with bare-metal kernel"
	@if [[ -z "$(DTB)" ]]; then \
		echo "No DTB found. Put one into out/ or archive/, or pass DTB=..." >&2; \
		echo "Tried: $(DTB_CANDIDATES)" >&2; \
		exit 2; \
	fi
	@# Enable semihosting-powered `poweroff` for QEMU runs.
	@$(MAKE) -C "$(AARCH64_DIR)" CROSS="$(AARCH64_CROSS)" clean
	@$(MAKE) -C "$(AARCH64_DIR)" CROSS="$(AARCH64_CROSS)" USERPROG="$(USERPROG)" KERNEL_DEFS="-DQEMU_SEMIHOSTING" all
	@bash tools/run-qemu-raspi3b.sh \
		--kernel "$(AARCH64_IMG)" \
		--dtb "$(DTB)" \
		--mem "$(MEM)"

run: run-aarch64

clean:
	@rm -rf "$(AARCH64_DIR)/build" "$(USERLAND_DIR)/build"