# Minimal workflow (bare-metal kernel + userland) for QEMU raspi3b (Pi Zero 2 W-ish)
#
# Targets:
#   make (all)  - build bare-metal kernel + userland
#   make run    - boot interactive QEMU (default: framebuffer + USB keyboard + UART logs on stdio)
#   make test   - boot bare-metal kernel in QEMU, run selftests, exit
#   make clean  - remove all build artifacts
#
# `make run` can be customized via variables:
#   GFX=0|1            (default: 1)
#   USB_KBD=0|1        (default: 1)
#   USB_NET=0|1        (default: 0)  # when USB_KBD=1 we default this off for deterministic enum
#   SERIAL=stdio|vc|null  (default: stdio)
#   MONITOR=none|stdio|vc (default: none)
#   USB_KBD_DEBUG=0|1  (default: 0)  # enables extra kernel USB debug logs
#
# Example:
#   make run SERIAL=vc

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

# Framebuffer request (used by `make run-gfx`).
FB_W ?= 1920
FB_H ?= 1080
FB_BPP ?= 32
# Virtual framebuffer height multiplier (reduces wrap/reset frequency in termfb scrolling).
# Larger values use more RAM but make long outputs smoother.
FB_VIRT_MULT ?= 4

# QEMU display backend for graphics runs.
# - macOS: cocoa
# - Linux: gtk (cocoa is macOS-only)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
QEMU_DISPLAY ?= cocoa
else
QEMU_DISPLAY ?= gtk
endif

# Optional: which userland tool to embed as the EL0 payload.
USERPROG ?= init


# --- `make run` defaults (interactive) ---
GFX ?= 1
USB_KBD ?= 1
USB_NET ?= 0
SERIAL ?= stdio
MONITOR ?= none
USB_KBD_DEBUG ?= 0


.PHONY: all run test clean help
.PHONY: aarch64-kernel test-aarch64 userland

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

run: userland
	@echo "Starting QEMU (raspi3b) interactive run"
	@echo "  GFX=$(GFX) USB_KBD=$(USB_KBD) USB_NET=$(USB_NET) SERIAL=$(SERIAL) MONITOR=$(MONITOR)"
	@if [[ -z "$(DTB)" ]]; then \
		echo "No DTB found. Put one into out/ or archive/, or pass DTB=..." >&2; \
		echo "Tried: $(DTB_CANDIDATES)" >&2; \
		exit 2; \
	fi
	@kdefs="-DQEMU_SEMIHOSTING"; \
	if [[ "$(GFX)" == "1" ]]; then \
		virt_h=$$(( $(FB_H) * $(FB_VIRT_MULT) )); \
		kdefs+=" -DENABLE_FB -DFB_REQ_W=$(FB_W) -DFB_REQ_H=$(FB_H) -DFB_REQ_VIRT_H=$$virt_h -DFB_REQ_BPP=$(FB_BPP)"; \
	fi; \
	if [[ "$(USB_KBD)" == "1" ]]; then \
		kdefs+=" -DENABLE_USB_KBD"; \
		if [[ "$(USB_KBD_DEBUG)" == "1" ]]; then kdefs+=" -DENABLE_USB_KBD_DEBUG"; fi; \
	fi; \
	$(MAKE) -C "$(AARCH64_DIR)" CROSS="$(AARCH64_CROSS)" USERPROG="$(USERPROG)" KERNEL_DEFS="$$kdefs" all
	@args=( --kernel "$(AARCH64_IMG)" --dtb "$(DTB)" --mem "$(MEM)" ); \
	if [[ "$(GFX)" == "1" ]]; then \
		args+=( --gfx --display "$(QEMU_DISPLAY)" --serial "$(SERIAL)" --monitor "$(MONITOR)" ); \
	fi; \
	if [[ "$(USB_NET)" == "0" ]]; then args+=( --no-usb-net ); fi; \
	if [[ "$(USB_KBD)" == "1" ]]; then args+=( --usb-kbd ); fi; \
	set +e; bash tools/run-qemu-raspi3b.sh "$${args[@]}"; rc=$$?; if [[ $$rc -eq 0 || $$rc -eq 128 ]]; then exit 0; fi; exit $$rc

test:
	@$(MAKE) USERPROG=kinit test-aarch64

test-aarch64: userland
	@echo "Starting QEMU (raspi3b) selftests"
	@if [[ -z "$(DTB)" ]]; then \
		echo "No DTB found. Put one into out/ or archive/, or pass DTB=..." >&2; \
		echo "Tried: $(DTB_CANDIDATES)" >&2; \
		exit 2; \
	fi
	@# Enable semihosting-powered exit codes for QEMU tests.
	@$(MAKE) -C "$(AARCH64_DIR)" CROSS="$(AARCH64_CROSS)" USERPROG="$(USERPROG)" KERNEL_DEFS="-DQEMU_SEMIHOSTING" all
	@set +e; bash tools/run-qemu-raspi3b.sh --kernel "$(AARCH64_IMG)" --dtb "$(DTB)" --mem "$(MEM)"; rc=$$?; if [[ $$rc -eq 0 || $$rc -eq 128 ]]; then exit 0; fi; exit $$rc

clean:
	@rm -rf "$(AARCH64_DIR)/build" "$(USERLAND_DIR)/build"

help:
	@echo "Targets:"; \
	echo "  make (all)       Build kernel + userland"; \
	echo "  make run         Interactive QEMU run (default: framebuffer+usb-kbd+stdio)"; \
	echo "  make test        QEMU selftests (headless)"; \
	echo "  make clean       Remove build artifacts"; \
	echo ""; \
	echo "Run variables (defaults shown):"; \
	echo "  GFX=$(GFX) USB_KBD=$(USB_KBD) USB_NET=$(USB_NET) SERIAL=$(SERIAL) MONITOR=$(MONITOR) USB_KBD_DEBUG=$(USB_KBD_DEBUG)"; \
	echo "  FB_W=$(FB_W) FB_H=$(FB_H) FB_BPP=$(FB_BPP) FB_VIRT_MULT=$(FB_VIRT_MULT)"; \
	echo ""; \
	echo "Examples:"; \
	echo "  make run SERIAL=vc"; \
	echo "  make run GFX=0 USB_KBD=0"; \
	echo "  make run USB_KBD_DEBUG=1"; \
	echo "  make run FB_VIRT_MULT=8"