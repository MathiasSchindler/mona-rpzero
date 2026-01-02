#!/usr/bin/env bash
set -euo pipefail

# Automated IPv6 bringup diagnostics test.
#
# Brings up the TAP+dnsmasq RA/RDNSS environment, runs QEMU with the USB RNDIS NIC
# on that TAP, and expects the guest `net6test` program to print PASS.
#
# This is intentionally opinionated: it captures only the signals we need.

IFNAME="${1:-${TAP_IF:-mona0}}"
TEST_TIMEOUT_S="${TEST_TIMEOUT_S:-15}"
OWNER_USER="${SUDO_USER:-$USER}"
START_SECS=$SECONDS

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STATE_DIR="/tmp/mona-tap6-${IFNAME}"
LOGFILE="${STATE_DIR}/qemu-net6test.log"
ARCHIVE_STEM="/tmp/mona-tap6-${IFNAME}-$(date +%Y%m%d-%H%M%S)"

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

qpid=""

kill_pid() {
  local pid="$1"
  if [[ -z "$pid" ]]; then
    return 0
  fi
  if ! kill -0 "$pid" 2>/dev/null; then
    return 0
  fi
  kill -TERM "$pid" >/dev/null 2>&1 || true
  local i
  for i in $(seq 1 40); do
    if ! kill -0 "$pid" 2>/dev/null; then
      return 0
    fi
    sleep 0.05
  done
  kill -KILL "$pid" >/dev/null 2>&1 || true
}

cleanup() {
  kill_pid "$qpid"
  # tap6-down may remove $STATE_DIR; preserve the QEMU UART log for debugging.
  if [[ -f "$LOGFILE" ]]; then
    cp -f "$LOGFILE" "${ARCHIVE_STEM}.log" >/dev/null 2>&1 || true
  fi
  sudo env MONA_TAP_DEBUG="${MONA_TAP_DEBUG:-1}" MONA_ENABLE_NAT66="${MONA_ENABLE_NAT66:-0}" tools/tap6-down.sh "$IFNAME" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "[test-net6] bringing up TAP $IFNAME (debug capture enabled)"
sudo env MONA_TAP_DEBUG="${MONA_TAP_DEBUG:-1}" MONA_ENABLE_NAT66="${MONA_ENABLE_NAT66:-0}" \
  MONA_RA_MIN_S="${MONA_RA_MIN_S:-1}" MONA_RA_MAX_S="${MONA_RA_MAX_S:-5}" \
  tools/tap6-up.sh "$IFNAME" "$OWNER_USER" >/dev/null

# tap6-up runs as root and may end up owning the state dir.
# Make it user-writable so this script can create temporary files (FIFO, logs).
sudo chown "$OWNER_USER":"$OWNER_USER" "$STATE_DIR" >/dev/null 2>&1 || true
sudo chmod 0777 "$STATE_DIR" >/dev/null 2>&1 || true

echo "[test-net6] running QEMU (log: $LOGFILE)"

remaining=$((TEST_TIMEOUT_S - (SECONDS - START_SECS)))
if (( remaining < 1 )); then
  remaining=1
fi

QEMU_CMD=(bash tools/run-qemu-raspi3b.sh \
  --kernel "$KERNEL_IMG" \
  --dtb "$DTB" \
  --mem "${MEM:-1024}" \
  --usb-net-backend tap \
  --tap-if "$IFNAME" \
  --serial "file:$LOGFILE" \
  --monitor none)

: >"$LOGFILE"

set +e
"${QEMU_CMD[@]}" &
qpid=$!
set -e

deadline=$((SECONDS + remaining))
passed=0

while (( SECONDS < deadline )); do
  if grep -q "\[net6test\] PASS" "$LOGFILE" 2>/dev/null; then
    passed=1
    break
  fi
  if ! kill -0 "$qpid" 2>/dev/null; then
    break
  fi
  sleep 0.05
done

if [[ "$passed" == "1" ]]; then
  kill_pid "$qpid"
  wait "$qpid" >/dev/null 2>&1 || true
  echo "[test-net6] PASS"
  exit 0
fi

if kill -0 "$qpid" 2>/dev/null; then
  echo "[test-net6] TIMEOUT: QEMU exceeded ${TEST_TIMEOUT_S}s" >&2
  kill_pid "$qpid"
fi

set +e
wait "$qpid"
qrc=$?
set -e

echo "[test-net6] QEMU exit code: $qrc"

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
