#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  tools/run-qemu-raspi3b.sh \
    [--sd sd.img] \
    --kernel path/to/kernel8.img \
    --dtb path/to/bcm2710-rpi-3-b.dtb \
    [--usb-kbd] \
    [--no-usb-net] \
    [--usb-net-backend user|tap] \
    [--tap-if name] \
    [--serial stdio|vc|null] \
    [--monitor none|stdio|vc] \
    --append "console=ttyAMA0 root=/dev/mmcblk0p2 rootwait"

Notes:
  - QEMU `-M raspi3b` loads the kernel via `-kernel` (not via Pi firmware).
  - For QEMU `raspi3b`, a matching DTB like `bcm2710-rpi-3-b.dtb` is the safest default.
    A Zero 2 W DTB (`bcm2710-rpi-zero-2-w.dtb`) is useful later for real hardware.
  - `--append` is passed as Linux kernel command line; for a non-Linux kernel it
    may be ignored.
  - Exit QEMU: Ctrl+A then X.
  - `--serial vc` puts the UART into a QEMU in-window "virtual console" (handy on macOS,
    avoids relying on the terminal's stdin focus). If you don't see it immediately, switch
    to the virtual console inside the QEMU window (often Ctrl-Alt-2; on macOS this may be
    Ctrl-Option-2 depending on your keyboard settings).
  - `--serial null` disables the UART backend (useful for validating non-UART input paths).
  - When `--serial vc` is used, this script disables the QEMU monitor by default so that the
    console switch keys land on the UART. You can override with `--monitor stdio` or
    `--monitor vc`.
EOF
}

SD=""
KERNEL=""
DTB=""
APPEND=""
MEM=1024
GFX=0
DISPLAY_BACKEND=""
SERIAL_BACKEND=""
MONITOR_BACKEND=""
USB_KBD=0
USB_NET=1
USB_NET_BACKEND="user"
TAP_IF=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --sd) SD="$2"; shift 2;;
    --kernel) KERNEL="$2"; shift 2;;
    --dtb) DTB="$2"; shift 2;;
    --append) APPEND="$2"; shift 2;;
    --mem) MEM="$2"; shift 2;;
    --graphic|--gfx) GFX=1; shift;;
    --display) DISPLAY_BACKEND="$2"; GFX=1; shift 2;;
    --usb-kbd) USB_KBD=1; shift;;
    --no-usb-net) USB_NET=0; shift;;
    --usb-net-backend) USB_NET_BACKEND="$2"; shift 2;;
    --tap-if) TAP_IF="$2"; shift 2;;
    --serial) SERIAL_BACKEND="$2"; shift 2;;
    --monitor) MONITOR_BACKEND="$2"; shift 2;;
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
  # Keep UART enabled while also opening a display window.
  if [[ -z "$DISPLAY_BACKEND" ]]; then
    case "$(uname -s)" in
      Darwin) DISPLAY_BACKEND="cocoa";;
      *) DISPLAY_BACKEND="gtk";;
    esac
  fi
  if [[ -z "$SERIAL_BACKEND" ]]; then
    SERIAL_BACKEND="stdio"
  fi

  # In graphical mode, QEMU also creates a monitor. When using `-serial vc` we usually want
  # the in-window console switch (Ctrl-Alt/Option-2) to land on the UART, not the monitor.
  # Default to disabling the monitor unless explicitly requested.
  if [[ "$SERIAL_BACKEND" == "vc" && -z "$MONITOR_BACKEND" ]]; then
    MONITOR_BACKEND="none"
  fi

  # If we use the in-window serial console, ensure the GUI makes console switching discoverable.
  # Note: `show-tabs` is supported by gtk/sdl displays (not cocoa).
  if [[ "$SERIAL_BACKEND" == "vc" ]]; then
    case "$DISPLAY_BACKEND" in
      gtk*|sdl*)
        if [[ "$DISPLAY_BACKEND" != *"show-tabs="* ]]; then
          DISPLAY_BACKEND+="${DISPLAY_BACKEND:+,}show-tabs=on"
        fi
        ;;
    esac
  fi

  case "$SERIAL_BACKEND" in
    stdio|vc|null)
      cmd+=( -display "$DISPLAY_BACKEND" -serial "$SERIAL_BACKEND" )
      ;;
    *)
      echo "Unknown --serial backend: $SERIAL_BACKEND (expected: stdio|vc|null)" >&2
      exit 2
      ;;
  esac

  if [[ -n "$MONITOR_BACKEND" ]]; then
    case "$MONITOR_BACKEND" in
      none|stdio|vc)
        cmd+=( -monitor "$MONITOR_BACKEND" )
        ;;
      *)
        echo "Unknown --monitor backend: $MONITOR_BACKEND (expected: none|stdio|vc)" >&2
        exit 2
        ;;
    esac
  fi
else
  cmd+=( -nographic )
fi

if [[ -n "$SD" ]]; then
  cmd+=( -drive "file=${SD},format=raw,if=sd" )
fi

if [[ -n "$APPEND" ]]; then
  cmd+=( -append "$APPEND" )
fi

if [[ "$USB_KBD" -eq 1 ]]; then
  cmd+=( -device usb-kbd )
fi

if [[ "$USB_NET" -eq 1 ]]; then
  case "$USB_NET_BACKEND" in
    user)
      # Optional: user-mode networking. Enable IPv6 explicitly.
      cmd+=( -netdev user,id=net0,ipv6=on -device usb-net,netdev=net0 )
      ;;
    tap)
      if [[ -z "$TAP_IF" ]]; then
        echo "--usb-net-backend tap requires --tap-if <name>" >&2
        exit 2
      fi
      # Host TAP networking for realistic L2 IPv6 tests (RA/SLAAC/NDP).
      cmd+=( -netdev tap,id=net0,ifname="$TAP_IF",script=no,downscript=no -device usb-net,netdev=net0 )
      ;;
    *)
      echo "Unknown --usb-net-backend: $USB_NET_BACKEND (expected: user|tap)" >&2
      exit 2
      ;;
  esac
fi

exec "${cmd[@]}"