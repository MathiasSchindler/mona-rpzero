#!/usr/bin/env bash
set -euo pipefail

# Bring up a host TAP interface for mona-rpzero and run RA (RDNSS) + DNS on it.
#
# This is intended for Linux hosts where QEMU "slirp" IPv6 is unreliable for ICMPv6/UDP.
#
# Usage:
#   sudo tools/tap6-up.sh [tap_if] [owner_user]
#
# Optional env vars:
#   MONA_TAP_IF        (default: arg1 or mona0)
#   MONA_TAP_OWNER     (default: arg2 or $SUDO_USER or $USER)
#   MONA_PREFIX64      (default: fd42:6d6f:6e61:1::/64)
#   MONA_HOST_IP       (default: fd42:6d6f:6e61:1::1)
#   MONA_ENABLE_NAT66  (default: 0)
#   MONA_WAN_IF        (default: auto-detected from the IPv6 default route)
#   MONA_TAP_DEBUG     (default: 0) Set to 1 to dump RA packets to pcap.
#
# After running this:
#   make run USB_NET=1 USB_NET_BACKEND=tap TAP_IF=<tap_if>
#
# Inside mona:
#   ping6 <host-ipv6>
#   dns6 www.google.com

IFNAME="${MONA_TAP_IF:-${1:-mona0}}"
OWNER="${MONA_TAP_OWNER:-${2:-${SUDO_USER:-$USER}}}"
PREFIX64="${MONA_PREFIX64:-fd42:6d6f:6e61:1::/64}"
HOST_IP="${MONA_HOST_IP:-fd42:6d6f:6e61:1::1}"
ENABLE_NAT66="${MONA_ENABLE_NAT66:-0}"
DEBUG="${MONA_TAP_DEBUG:-0}"
RA_MIN_S="${MONA_RA_MIN_S:-10}"
RA_MAX_S="${MONA_RA_MAX_S:-1200}"

STATE_DIR="/tmp/mona-tap6-${IFNAME}"
DNSMASQ_PIDFILE="$STATE_DIR/dnsmasq.pid"
DNSMASQ_DUMPFILE="$STATE_DIR/dnsmasq.pcap"
DNSMASQ_RESTART_PIDFILE="$STATE_DIR/dnsmasq-restarter.pid"
TCPDUMP_PIDFILE="$STATE_DIR/tcpdump.pid"
TCPDUMP_PCAPFILE="$STATE_DIR/tap.pcap"

if [[ $EUID -ne 0 ]]; then
  echo "This script must run as root." >&2
  echo "Try: sudo $0 $IFNAME $OWNER" >&2
  exit 2
fi

mkdir -p "$STATE_DIR"

# Create the TAP device owned by the target user.
"$(dirname "$0")/setup-tap.sh" "$IFNAME" "$OWNER"

# Assign host IPv6 on the TAP.
# (Don't fail if it already exists.)
ip -6 addr add "$HOST_IP/64" dev "$IFNAME" nodad 2>/dev/null || true

# Ensure a stable link-local address exists even if the TAP has no carrier yet.
# Some RA daemons (including dnsmasq) behave poorly if the interface has no
# usable link-local address at startup.
if ! ip -6 addr show dev "$IFNAME" scope link 2>/dev/null | grep -q "inet6 fe80:"; then
  ip -6 addr add "fe80::1/64" dev "$IFNAME" nodad 2>/dev/null || true
fi
ip link set "$IFNAME" up

# Become an IPv6 router for the TAP network.
sysctl -w net.ipv6.conf.all.forwarding=1 >/dev/null
sysctl -w "net.ipv6.conf.${IFNAME}.accept_ra=0" >/dev/null || true

# Start dnsmasq bound to the TAP.
if ! command -v dnsmasq >/dev/null 2>&1; then
  cat >&2 <<EOF
ERROR: dnsmasq not found.
Install it and re-run.

Debian/Ubuntu:
  sudo apt-get install dnsmasq
EOF
  exit 1
fi

cat >"$STATE_DIR/dnsmasq.conf" <<EOF
# mona TAP IPv6 router (RA/RDNSS) + DNS forwarder
interface=$IFNAME
bind-interfaces
listen-address=$HOST_IP
port=53
# Send Router Advertisements for SLAAC on this interface.
enable-ra
# Make RA frequent so the guest configures quickly.
ra-param=$IFNAME,${RA_MIN_S},${RA_MAX_S}
# Use the interface address/prefix as the advertised /64.
dhcp-range=::,constructor:$IFNAME,slaac,64,10m
# Advertise the DNS server via RDNSS (RFC 8106).
dhcp-option=option6:dns-server,[$HOST_IP]
# Use host resolv.conf by default (do not set no-resolv)
domain-needed
bogus-priv
EOF

if [[ "$DEBUG" == "1" ]]; then
  cat >>"$STATE_DIR/dnsmasq.conf" <<EOF
# Debugging: dump Router Advertisements to a pcap file.
dumpfile=$DNSMASQ_DUMPFILE
dumpmask=0x4000
log-debug
EOF
fi

# Optional: full link capture (RS/RA/NS/NA + DNS) for debugging.
if [[ "$DEBUG" == "1" ]]; then
  if command -v tcpdump >/dev/null 2>&1; then
    if [[ -f "$TCPDUMP_PIDFILE" ]]; then
      oldpid="$(cat "$TCPDUMP_PIDFILE" || true)"
      if [[ -n "$oldpid" ]]; then
        kill "$oldpid" 2>/dev/null || true
      fi
    fi

    # Capture ICMPv6 (NDP/RA/ping) and DNS, without name resolution.
    # Run in background and record pid for teardown.
    tcpdump -n -U -i "$IFNAME" -w "$TCPDUMP_PCAPFILE" '(icmp6 or (udp port 53))' >/dev/null 2>&1 &
    echo $! >"$TCPDUMP_PIDFILE"
  else
    echo "WARN: tcpdump not found; MONA_TAP_DEBUG will only dump dnsmasq RAs." >&2
  fi
fi

# Stop previous instance if present.
if [[ -f "$DNSMASQ_PIDFILE" ]]; then
  oldpid="$(cat "$DNSMASQ_PIDFILE" || true)"
  if [[ -n "$oldpid" ]]; then
    kill "$oldpid" 2>/dev/null || true
  fi
fi

# Stop any previous dnsmasq restarter if present.
if [[ -f "$DNSMASQ_RESTART_PIDFILE" ]]; then
  oldpid="$(cat "$DNSMASQ_RESTART_PIDFILE" || true)"
  if [[ -n "$oldpid" ]]; then
    kill "$oldpid" 2>/dev/null || true
  fi
fi

# dnsmasq daemonizes by default.
dnsmasq --conf-file="$STATE_DIR/dnsmasq.conf" --pid-file="$DNSMASQ_PIDFILE"

# If dnsmasq started before QEMU attaches to the TAP, the interface may appear
# "down" (no carrier) and some kernels will keep addresses tentative / delay
# link-local autoconf. In that case, dnsmasq may fail to emit RAs.
#
# Workaround: once the interface becomes usable, restart dnsmasq once.
needs_restart=0
if ! ip -6 addr show dev "$IFNAME" scope link 2>/dev/null | grep -q "inet6 fe80:"; then
  needs_restart=1
fi

if [[ "$needs_restart" == "1" ]]; then
  (
    set +e
    for _ in $(seq 1 100); do
      if ip -6 addr show dev "$IFNAME" scope link 2>/dev/null | grep -q "inet6 fe80:"; then
        pid="$(cat "$DNSMASQ_PIDFILE" 2>/dev/null || true)"
        if [[ -n "$pid" ]]; then
          kill "$pid" 2>/dev/null || true
        fi
        rm -f "$DNSMASQ_PIDFILE" 2>/dev/null || true
        dnsmasq --conf-file="$STATE_DIR/dnsmasq.conf" --pid-file="$DNSMASQ_PIDFILE" 2>/dev/null || true
        exit 0
      fi
      sleep 0.05
    done
    exit 0
  ) &
  echo $! >"$DNSMASQ_RESTART_PIDFILE"
fi

WAN_IF="${MONA_WAN_IF:-}"
if [[ "$ENABLE_NAT66" == "1" ]]; then
  if [[ -z "$WAN_IF" ]]; then
    WAN_IF="$(ip -6 route show default 2>/dev/null | head -n1 | awk '{for(i=1;i<=NF;i++) if($i=="dev") {print $(i+1); exit}}')"
  fi

  if [[ -z "$WAN_IF" ]]; then
    echo "WARN: could not auto-detect WAN interface for NAT66; set MONA_WAN_IF." >&2
  else
    if command -v nft >/dev/null 2>&1; then
      # Create a dedicated table so teardown is easy.
      nft list table ip6 mona_nat >/dev/null 2>&1 || nft add table ip6 mona_nat
      nft list chain ip6 mona_nat postrouting >/dev/null 2>&1 || nft 'add chain ip6 mona_nat postrouting { type nat hook postrouting priority srcnat; policy accept; }'

      # Remove any prior matching rule (best-effort).
      nft -a list chain ip6 mona_nat postrouting 2>/dev/null | grep -q "mona-tap6" && true

      # Tag with a comment for humans.
      nft add rule ip6 mona_nat postrouting oifname "$WAN_IF" ip6 saddr "$PREFIX64" masquerade comment "mona-tap6"
    else
      cat >&2 <<EOF
WARN: nft not found; NAT66 not configured.
Install nftables or set MONA_ENABLE_NAT66=0.
EOF
    fi
  fi
fi

cat <<EOF
OK: TAP IPv6 network is up

- TAP:        $IFNAME (owner: $OWNER)
- Host IPv6:  $HOST_IP/64
- Prefix:     $PREFIX64 (advertised via RA)
- DNS:        $HOST_IP (dnsmasq)
- NAT66:      $ENABLE_NAT66
 - Debug:      $DEBUG
 - Pcap:       $TCPDUMP_PCAPFILE
 - dnsmasq pcap (RA-only): $DNSMASQ_DUMPFILE

Next (QEMU):
  make run USB_NET=1 USB_NET_BACKEND=tap TAP_IF=$IFNAME

Teardown:
  sudo tools/tap6-down.sh $IFNAME
EOF
