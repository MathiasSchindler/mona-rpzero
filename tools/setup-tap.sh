#!/usr/bin/env bash
set -euo pipefail

IFNAME="${1:-mona0}"
OWNER="${2:-${SUDO_USER:-$USER}}"

if [[ -z "$IFNAME" ]]; then
  echo "usage: $0 <tap-ifname> [owner-user]" >&2
  exit 2
fi

if [[ $EUID -ne 0 ]]; then
  echo "This script must run as root (needs to create a TAP device)." >&2
  echo "Try: sudo $0 $IFNAME $OWNER" >&2
  exit 2
fi

# Check for macOS
if [[ "$(uname -s)" == "Darwin" ]]; then
  echo "Error: This script relies on Linux-specific tools (ip, modprobe) and kernel modules." >&2
  echo "On macOS, consider using 'make run USB_NET_BACKEND=user' (slirp) or install TunTap." >&2
  exit 1
fi

if [[ ! -e /dev/net/tun ]]; then
  echo "/dev/net/tun missing; trying to load tun module" >&2
  modprobe tun || true
fi

if [[ ! -e /dev/net/tun ]]; then
  echo "ERROR: /dev/net/tun still missing. Is the tun module available?" >&2
  exit 2
fi

# If it exists already, delete and recreate to ensure correct owner/mode.
if ip link show "$IFNAME" >/dev/null 2>&1; then
  ip link set "$IFNAME" down || true
  ip link del "$IFNAME" || true
fi

ip tuntap add dev "$IFNAME" mode tap user "$OWNER"
ip link set "$IFNAME" up

cat <<EOF
OK: created TAP interface '$IFNAME' owned by '$OWNER'

Next:
  make run USB_NET=1 USB_NET_BACKEND=tap TAP_IF=$IFNAME

Notes:
  - For L2 IPv6 tests (NDP/RA), you typically also configure IPv6 on the host side,
    e.g. 'ip -6 addr add fd00::1/64 dev $IFNAME' and/or run a RA daemon.
EOF
