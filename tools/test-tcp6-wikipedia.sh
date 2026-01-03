#!/usr/bin/env bash
set -euo pipefail

# Phase 0 TLS bringup test: DNS AAAA + TCP connect to de.wikipedia.org:443 over IPv6.
#
# Brings up TAP+dnsmasq RA/RDNSS + optional NAT66, runs QEMU with the USB RNDIS NIC,
# and expects the guest `tcp6test` payload to print PASS.

IFNAME="${1:-${TAP_IF:-mona0}}"
TEST_TIMEOUT_S="${TEST_TIMEOUT_S:-25}"
OWNER_USER="${SUDO_USER:-$USER}"
START_SECS=$SECONDS

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STATE_DIR="/tmp/mona-tap6-${IFNAME}"
LOGFILE="${STATE_DIR}/qemu-tcp6test.log"
ARCHIVE_STEM="/tmp/mona-tap6-${IFNAME}-$(date +%Y%m%d-%H%M%S)"

cd "$ROOT_DIR"

if [[ -z "${DTB:-}" ]]; then
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
  if [[ -z "$pid" ]]; then return 0; fi
  if ! kill -0 "$pid" 2>/dev/null; then return 0; fi
  kill -TERM "$pid" >/dev/null 2>&1 || true
  local i
  for i in $(seq 1 40); do
    if ! kill -0 "$pid" 2>/dev/null; then return 0; fi
    sleep 0.05
  done
  kill -KILL "$pid" >/dev/null 2>&1 || true
}

cleanup() {
  kill_pid "$qpid"
  if [[ -f "$LOGFILE" ]]; then
    cp -f "$LOGFILE" "${ARCHIVE_STEM}.log" >/dev/null 2>&1 || true
  fi
  sudo env MONA_TAP_DEBUG="${MONA_TAP_DEBUG:-1}" MONA_ENABLE_NAT66="${MONA_ENABLE_NAT66:-1}" tools/tap6-down.sh "$IFNAME" >/dev/null 2>&1 || true
}
trap cleanup EXIT

# NAT66 is required to reach the public internet from the guest.
echo "[test-tcp6-wikipedia] bringing up TAP $IFNAME (NAT66=${MONA_ENABLE_NAT66:-1})"
sudo env MONA_TAP_DEBUG="${MONA_TAP_DEBUG:-1}" MONA_ENABLE_NAT66="${MONA_ENABLE_NAT66:-1}" \
  MONA_RA_MIN_S="${MONA_RA_MIN_S:-1}" MONA_RA_MAX_S="${MONA_RA_MAX_S:-5}" \
  tools/tap6-up.sh "$IFNAME" "$OWNER_USER" >/dev/null

sudo chown "$OWNER_USER":"$OWNER_USER" "$STATE_DIR" >/dev/null 2>&1 || true
sudo chmod 0777 "$STATE_DIR" >/dev/null 2>&1 || true

echo "[test-tcp6-wikipedia] running QEMU (log: $LOGFILE)"

remaining=$((TEST_TIMEOUT_S - (SECONDS - START_SECS)))
if (( remaining < 1 )); then remaining=1; fi

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
  if grep -q "\[tcp6test\] PASS" "$LOGFILE" 2>/dev/null; then
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
  echo "[test-tcp6-wikipedia] PASS"
  exit 0
fi

if kill -0 "$qpid" 2>/dev/null; then
  echo "[test-tcp6-wikipedia] TIMEOUT: QEMU exceeded ${TEST_TIMEOUT_S}s" >&2
  kill_pid "$qpid"
fi

set +e
wait "$qpid"
qrc=$?
set -e

echo "[test-tcp6-wikipedia] QEMU exit code: $qrc"

echo "[test-tcp6-wikipedia] FAIL: guest did not report PASS" >&2

grep -n "\[tcp6test\]" "$LOGFILE" >&2 || true

exit 1
