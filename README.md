# mona-rpzero

This repository contains an educational **bare-metal AArch64 kernel** project targeting the **Raspberry Pi Zero 2 W** (BCM2710A1), developed **QEMU-first** using `qemu-system-aarch64 -M raspi3b` for fast iteration.

The long-term direction is to provide a small, Linux-inspired environment with a **Linux-like AArch64 syscall ABI** so that a curated set of simple Linux AArch64 programs can run (initially focusing on static binaries and a minimal userland/initramfs workflow).

## Project plan

For the detailed roadmap, design constraints, and current status, see: [plan.md](plan.md)

## Notes

- Large parts of the code in this repository are **LLM-generated** (model: **GPT-5.2**), with human review and iteration.
- This project is released under **CC0-1.0** (public domain dedication). 
- The `userland/` programs are built as **freestanding** binaries (no glibc / no standard C library) and use raw syscalls via `svc #0`.