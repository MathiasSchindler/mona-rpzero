#!/usr/bin/env bash
set -euo pipefail

# Automated IPv6 bringup diagnostics test.
#
# Brings up the TAP+dnsmasq RA/RDNSS environment, runs QEMU with the USB RNDIS NIC
# on that TAP, and expects the guest `net6test` program to print PASS.
#
# This is intentionally opinionated: it captures only the signals we need.

IFNAME="${1:-${TAP_IF:-mona0}}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STATE_DIR="/tmp/mona-tap6-${IFNAME}"
LOGFILE="${STATE_DIR}/qemu-net6test.log"

cd "$ROOT_DIR"

if [[ -z "${DTB:-}" ]]; then
  # Match top-level Makefile default candidate list.
  for cand in out/bcm2710-rpi-3-b.dtb out/bcm2710-rpi-zero-2-w.dtb archive/bcm2710-rpi-3-b.dtb archive/bcm2710-rpi-zero-2-w.dtb; do
    if [[ -f "$cand" ]]; then
      DTB="$cand"
      break
    fi
  done
fi

if [[ -z "${DTB:-}" || ! -f "$DTB" ]]; then
  echo "ERROR: DTB not found; pass DTB=..." >&2
  exit 2
fi

KERNEL_IMG="${KERNEL_IMG:-kernel-aarch64/build/kernel8.img}"
if [[ ! -f "$KERNEL_IMG" ]]; then
  echo "ERROR: kernel image not found at $KERNEL_IMG (build first)" >&2
  exit 2
fi

mkdir -p "$STATE_DIR"

cleanup() {
  sudo env MONA_TAP_DEBUG="${MONA_TAP_DEBUG:-1}" MONA_ENABLE_NAT66="${MONA_ENABLE_NAT66:-0}" tools/tap6-down.sh "$IFNAME" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "[test-net6] bringing up TAP $IFNAME (debug capture enabled)"
sudo env MONA_TAP_DEBUG="${MONA_TAP_DEBUG:-1}" MONA_ENABLE_NAT66="${MONA_ENABLE_NAT66:-0}" tools/tap6-up.sh "$IFNAME" "${SUDO_USER:-$USER}" >/dev/null

echo "[test-net6] running QEMU (log: $LOGFILE)"
set +e
(
  bash tools/run-qemu-raspi3b.sh \
    --kernel "$KERNEL_IMG" \
    --dtb "$DTB" \
    --mem "${MEM:-1024}" \
    --usb-net-backend tap \
    --tap-if "$IFNAME" \
    --serial stdio \
    --monitor none \
    2>&1 | tee "$LOGFILE"
)
qrc=${PIPESTATUS[0]}
set -e

echo "[test-net6] QEMU exit code: $qrc"

if grep -q "\[net6test\] PASS" "$LOGFILE"; then
  echo "[test-net6] PASS"
  exit 0
fi

echo "[test-net6] FAIL: guest did not report PASS" >&2
echo "[test-net6] extracting guest net6test output:" >&2
grep -n "\[net6test\]" "$LOGFILE" >&2 || true

if command -v tcpdump >/dev/null 2>&1 && [[ -f "$STATE_DIR/tap.pcap" ]]; then
  echo "[test-net6] pcap summary (RS/RA/NS/NA/echo):" >&2
  # icmp6 type is at offset 40 in IPv6.
  tcpdump -nn -r "$STATE_DIR/tap.pcap" 'icmp6' 2>/dev/null | \
    awk '{print $0}' | \
    head -n 40 >&2 || true
fi

exit 1
