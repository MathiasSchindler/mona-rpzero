#!/usr/bin/env bash
set -euo pipefail

# Host->guest ping6 integration test (TAP).
#
# Runs the same QEMU+TAP environment as test-net6, but while the guest is up it
# additionally pings the guest's SLAAC global IPv6 address from the host.
#
# This helps validate NDP correctness in the reverse direction (host resolving
# guest global address).

IFNAME="${1:-${TAP_IF:-mona0}}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STATE_DIR="/tmp/mona-tap6-${IFNAME}"
LOGFILE="${STATE_DIR}/qemu-net6test.log"

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

cleanup() {
  sudo env MONA_TAP_DEBUG="${MONA_TAP_DEBUG:-1}" MONA_ENABLE_NAT66="${MONA_ENABLE_NAT66:-0}" tools/tap6-down.sh "$IFNAME" >/dev/null 2>&1 || true
}
trap cleanup EXIT

extract_guest_global() {
  # ipv6: slaac global=fd42:... router=fe80:...
  local line
  line="$(grep -m1 'ipv6: slaac global=' "$LOGFILE" 2>/dev/null || true)"
  if [[ -z "$line" ]]; then
    return 1
  fi
  sed -n 's/.*global=\([^ ]*\).*/\1/p' <<<"$line" | head -n1
}

wait_for() {
  local seconds="$1"; shift
  local end=$((SECONDS + seconds))
  while (( SECONDS < end )); do
    if "$@"; then
      return 0
    fi
    sleep 0.05
  done
  return 1
}

echo "[test-net6-hostping] bringing up TAP $IFNAME (debug capture enabled)"
sudo env MONA_TAP_DEBUG="${MONA_TAP_DEBUG:-1}" MONA_ENABLE_NAT66="${MONA_ENABLE_NAT66:-0}" tools/tap6-up.sh "$IFNAME" "${SUDO_USER:-$USER}" >/dev/null

: >"$LOGFILE"

echo "[test-net6-hostping] starting QEMU (log: $LOGFILE)"
set +e

QEMU_CMD=(bash tools/run-qemu-raspi3b.sh \
  --kernel "$KERNEL_IMG" \
  --dtb "$DTB" \
  --mem "${MEM:-1024}" \
  --usb-net-backend tap \
  --tap-if "$IFNAME" \
  --serial stdio \
  --monitor none)

# When stdout is redirected to a file, QEMU output can be block-buffered.
# Use stdbuf if available to make the logfile update promptly.
if command -v stdbuf >/dev/null 2>&1; then
  stdbuf -oL -eL "${QEMU_CMD[@]}" >"$LOGFILE" 2>&1 &
else
  "${QEMU_CMD[@]}" >"$LOGFILE" 2>&1 &
fi
qpid=$!
set -e

# Wait for SLAAC so we know the guest global address.
if ! wait_for "20" grep -q 'ipv6: slaac global=' "$LOGFILE"; then
  echo "[test-net6-hostping] FAIL: guest did not report SLAAC within timeout" >&2
  tail -n 120 "$LOGFILE" >&2 || true
  exit 1
fi

guest_global="$(extract_guest_global || true)"
if [[ -z "$guest_global" ]]; then
  echo "[test-net6-hostping] FAIL: could not parse guest global IPv6" >&2
  tail -n 120 "$LOGFILE" >&2 || true
  exit 1
fi

echo "[test-net6-hostping] guest_global=$guest_global"

# Wait for the guest to complete its own net6test (guest->host ping) first.
# This typically makes the host learn the guest's global->MAC mapping, avoiding
# a dependency on multicast NS delivery to the guest.
if ! wait_for "30" grep -q '\[net6test\] PASS' "$LOGFILE"; then
  echo "[test-net6-hostping] FAIL: guest did not report PASS within timeout" >&2
  tail -n 160 "$LOGFILE" >&2 || true
  exit 1
fi

# Host -> guest ping. Use -I to bind to the TAP interface.
if command -v ping >/dev/null 2>&1; then
  set +e
  ping -6 -I "$IFNAME" -c 1 -W 1 "$guest_global" >/dev/null 2>&1
  prc=$?
  set -e
  if [[ $prc -ne 0 ]]; then
    echo "[test-net6-hostping] FAIL: host ping6 -> guest failed (rc=$prc)" >&2
    echo "[test-net6-hostping] host neigh table:" >&2
    ip -6 neigh show dev "$IFNAME" >&2 || true

    if command -v tcpdump >/dev/null 2>&1 && [[ -f "$STATE_DIR/tap.pcap" ]]; then
      echo "[test-net6-hostping] pcap summary (icmp6):" >&2
      tcpdump -nn -r "$STATE_DIR/tap.pcap" 'icmp6' 2>/dev/null | head -n 60 >&2 || true
    fi

    # Still wait for QEMU to exit so teardown captures files.
    wait "$qpid" >/dev/null 2>&1 || true
    exit 1
  fi
else
  echo "[test-net6-hostping] SKIP: host ping not available" >&2
fi

# Wait for QEMU to exit.
set +e
wait "$qpid"
qrc=$?
set -e

echo "[test-net6-hostping] QEMU exit code: $qrc"

# Accept standard semihosting exit statuses.
if [[ $qrc -ne 0 && $qrc -ne 112 && $qrc -ne 128 ]]; then
  echo "[test-net6-hostping] FAIL: unexpected QEMU exit code: $qrc" >&2
  tail -n 200 "$LOGFILE" >&2 || true
  exit 1
fi

if ! grep -q '\[net6test\] PASS' "$LOGFILE"; then
  echo "[test-net6-hostping] FAIL: guest did not report PASS" >&2
  grep -n '\[net6test\]' "$LOGFILE" >&2 || true
  exit 1
fi

echo "[test-net6-hostping] PASS"
