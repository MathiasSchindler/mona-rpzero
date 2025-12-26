#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  tools/run-qemu-raspi3b.sh \
    [--sd sd.img] \
    --kernel path/to/kernel8.img \
    --dtb path/to/bcm2710-rpi-zero-2-w.dtb \
    --append "console=ttyAMA0 root=/dev/mmcblk0p2 rootwait"

Notes:
  - QEMU `-M raspi3b` loads the kernel via `-kernel` (not via Pi firmware).
  - `--append` is passed as Linux kernel command line; for a non-Linux kernel it
    may be ignored.
  - Exit QEMU: Ctrl+A then X.
EOF
}

SD=""
KERNEL=""
DTB=""
APPEND=""
MEM=1024

while [[ $# -gt 0 ]]; do
  case "$1" in
    --sd) SD="$2"; shift 2;;
    --kernel) KERNEL="$2"; shift 2;;
    --dtb) DTB="$2"; shift 2;;
    --append) APPEND="$2"; shift 2;;
    --mem) MEM="$2"; shift 2;;
    -h|--help) usage; exit 0;;
    *) echo "Unknown argument: $1" >&2; usage; exit 2;;
  esac
done

if [[ -z "$KERNEL" || -z "$DTB" ]]; then
  echo "Missing required arguments." >&2
  usage
  exit 2
fi

if [[ -n "$SD" && ! -f "$SD" ]]; then echo "SD image not found: $SD" >&2; exit 2; fi
if [[ ! -f "$KERNEL" ]]; then echo "Kernel not found: $KERNEL" >&2; exit 2; fi
if [[ ! -f "$DTB" ]]; then echo "DTB not found: $DTB" >&2; exit 2; fi

cmd=(
  qemu-system-aarch64
  -M raspi3b
  -cpu cortex-a53
  -m "$MEM"
  -nographic
  -kernel "$KERNEL"
  -dtb "$DTB"
)

if [[ -n "$SD" ]]; then
  cmd+=( -drive "file=${SD},format=raw,if=sd" )
fi

if [[ -n "$APPEND" ]]; then
  cmd+=( -append "$APPEND" )
fi

# Optional: user-mode networking (works for Linux userland; your own kernel will need a driver)
cmd+=( -netdev user,id=net0 -device usb-net,netdev=net0 )

exec "${cmd[@]}"