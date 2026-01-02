#!/usr/bin/env bash
set -euo pipefail

# Tear down the TAP IPv6 network created by tools/tap6-up.sh.
#
# Usage:
#   sudo tools/tap6-down.sh [tap_if]

IFNAME="${1:-${MONA_TAP_IF:-mona0}}"
STATE_DIR="/tmp/mona-tap6-${IFNAME}"
DNSMASQ_PIDFILE="$STATE_DIR/dnsmasq.pid"
TCPDUMP_PIDFILE="$STATE_DIR/tcpdump.pid"

DNSMASQ_PCAPFILE="$STATE_DIR/dnsmasq.pcap"
TCPDUMP_PCAPFILE="$STATE_DIR/tap.pcap"

if [[ $EUID -ne 0 ]]; then
  echo "This script must run as root." >&2
  echo "Try: sudo $0 $IFNAME" >&2
  exit 2
fi

if [[ -f "$DNSMASQ_PIDFILE" ]]; then
  pid="$(cat "$DNSMASQ_PIDFILE" || true)"
  if [[ -n "$pid" ]]; then
    kill "$pid" 2>/dev/null || true
  fi
fi

if [[ -f "$TCPDUMP_PIDFILE" ]]; then
  pid="$(cat "$TCPDUMP_PIDFILE" || true)"
  if [[ -n "$pid" ]]; then
    kill "$pid" 2>/dev/null || true
  fi
fi

# Remove NAT66 table if we created it.
if command -v nft >/dev/null 2>&1; then
  nft delete table ip6 mona_nat >/dev/null 2>&1 || true
fi

# Delete the TAP itself (removes addresses/routes).
if ip link show "$IFNAME" >/dev/null 2>&1; then
  ip link set "$IFNAME" down 2>/dev/null || true
  ip link del "$IFNAME" 2>/dev/null || true
fi

# Preserve debug captures (they are very useful post-mortem).
ts="$(date +%Y%m%d-%H%M%S 2>/dev/null || true)"
if [[ -n "$ts" ]]; then
  if [[ -f "$TCPDUMP_PCAPFILE" ]]; then
    last="/tmp/mona-tap6-${IFNAME}-${ts}.pcap"
    mv "$TCPDUMP_PCAPFILE" "$last" 2>/dev/null || true
    echo "Saved pcap: $last"
  fi
  if [[ -f "$DNSMASQ_PCAPFILE" ]]; then
    last="/tmp/mona-tap6-${IFNAME}-dnsmasq-${ts}.pcap"
    mv "$DNSMASQ_PCAPFILE" "$last" 2>/dev/null || true
    echo "Saved dnsmasq pcap: $last"
  fi
fi

rm -rf "$STATE_DIR"

echo "OK: torn down TAP IPv6 network ($IFNAME)"
