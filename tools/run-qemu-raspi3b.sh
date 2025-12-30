#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  tools/run-qemu-raspi3b.sh \
    [--sd sd.img] \
    --kernel path/to/kernel8.img \
    --dtb path/to/bcm2710-rpi-3-b.dtb \
    --append "console=ttyAMA0 root=/dev/mmcblk0p2 rootwait"

Notes:
  - QEMU `-M raspi3b` loads the kernel via `-kernel` (not via Pi firmware).
  - For QEMU `raspi3b`, a matching DTB like `bcm2710-rpi-3-b.dtb` is the safest default.
    A Zero 2 W DTB (`bcm2710-rpi-zero-2-w.dtb`) is useful later for real hardware.
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
GFX=0
DISPLAY_BACKEND=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --sd) SD="$2"; shift 2;;
    --kernel) KERNEL="$2"; shift 2;;
    --dtb) DTB="$2"; shift 2;;
    --append) APPEND="$2"; shift 2;;
    --mem) MEM="$2"; shift 2;;
    --graphic|--gfx) GFX=1; shift;;
    --display) DISPLAY_BACKEND="$2"; GFX=1; shift 2;;
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
  -semihosting
  -kernel "$KERNEL"
  -dtb "$DTB"
)

if [[ "$GFX" -eq 1 ]]; then
  # Keep UART on stdio while enabling a display window.
  if [[ -z "$DISPLAY_BACKEND" ]]; then
    DISPLAY_BACKEND="cocoa"
  fi
  cmd+=( -display "$DISPLAY_BACKEND" -serial stdio )
else
  cmd+=( -nographic )
fi

if [[ -n "$SD" ]]; then
  cmd+=( -drive "file=${SD},format=raw,if=sd" )
fi

if [[ -n "$APPEND" ]]; then
  cmd+=( -append "$APPEND" )
fi

# Optional: user-mode networking (works for Linux userland; your own kernel will need a driver)
cmd+=( -netdev user,id=net0 -device usb-net,netdev=net0 )

exec "${cmd[@]}"