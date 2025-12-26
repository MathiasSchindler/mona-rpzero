# Minimal workflow (bare-metal kernel + userland) for QEMU raspi3b (Pi Zero 2 W-ish)
#
# Targets:
#   make clean  - remove all build artifacts
#   make        - build bare-metal kernel + userland
#   make run    - boot bare-metal kernel in QEMU

SHELL := /usr/bin/env bash

DTB ?= kernel/build/bcm2710-rpi-zero-2-w.dtb

# QEMU raspi3b currently requires exactly 1 GiB RAM.
MEM ?= 1024

# Optional: which userland tool to embed as the EL0 payload.
USERPROG ?= init


.PHONY: all run clean
.PHONY: aarch64-kernel run-aarch64 userland

all: aarch64-kernel

# --- Bare-metal AArch64 kernel (Phase 0) ---

AARCH64_DIR ?= kernel-aarch64
AARCH64_CROSS ?= aarch64-linux-gnu-
AARCH64_IMG := $(AARCH64_DIR)/build/kernel8.img

USERLAND_DIR ?= userland

userland:
	@$(MAKE) -C "$(USERLAND_DIR)" CROSS="$(AARCH64_CROSS)" all
	@$(MAKE) -C "$(USERLAND_DIR)" CROSS="$(AARCH64_CROSS)" initramfs


aarch64-kernel: userland
	@$(MAKE) -C "$(AARCH64_DIR)" CROSS="$(AARCH64_CROSS)" USERPROG="$(USERPROG)" all

run-aarch64: aarch64-kernel
	@echo "Starting QEMU (raspi3b) with bare-metal kernel"
	@bash tools/run-qemu-raspi3b.sh \
		--kernel "$(AARCH64_IMG)" \
		--dtb "$(DTB)" \
		--mem "$(MEM)"

run: run-aarch64

clean:
	@rm -rf "$(AARCH64_DIR)/build" "$(USERLAND_DIR)/build"