#!/usr/bin/env bash
set -u

# Network Stack Validation Script
# Implements the testing scenario defined in docs/validation_plan.md

IFNAME="${1:-mona0}"
TEST_TIMEOUT_S="${TEST_TIMEOUT_S:-20}"
LOGFILE="/tmp/mona-validate-${IFNAME}.log"
PCAPFILE="/tmp/mona-validate-${IFNAME}.pcap"

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

pass() { echo -e "${GREEN}[PASS]${NC} $1"; }
fail() { echo -e "${RED}[FAIL]${NC} $1"; }
info() { echo -e "${YELLOW}[INFO]${NC} $1"; }

cleanup() {
    if [[ -n "${QPID:-}" ]]; then
        kill "$QPID" 2>/dev/null || true
    fi
    if [[ -n "${TCPDUMP_PID:-}" ]]; then
        kill "$TCPDUMP_PID" 2>/dev/null || true
    fi
    # Keep artifacts for inspection
}
trap cleanup EXIT

info "Starting Network Validation on $IFNAME"

# --- Stage 1: Host Environment ---
info "Stage 1: Host Environment"

if ! ip link show "$IFNAME" >/dev/null 2>&1; then
    fail "Interface $IFNAME does not exist. Run tools/tap6-up.sh first."
    exit 1
fi
pass "Interface $IFNAME exists"

if ! ip -6 addr show dev "$IFNAME" | grep -q "fd42:6d6f:6e61:1::1"; then
    fail "Host IP fd42:6d6f:6e61:1::1 not found on $IFNAME."
    exit 1
fi
pass "Host IP configured"

if ! pgrep -f "dnsmasq.*$IFNAME" >/dev/null; then
    fail "dnsmasq not running for $IFNAME."
    exit 1
fi
pass "dnsmasq running"

# Start Packet Capture
info "Starting packet capture to $PCAPFILE..."
tcpdump -U -i "$IFNAME" -w "$PCAPFILE" -Z root ip6 >/dev/null 2>&1 &
TCPDUMP_PID=$!

# --- Start Guest ---
info "Starting Guest (QEMU)..."
: > "$LOGFILE"

# Use existing run script but capture PID
bash tools/run-qemu-raspi3b.sh \
  --kernel kernel-aarch64/build/kernel8.img \
  --dtb archive/bcm2710-rpi-zero-2-w.dtb \
  --usb-net-backend tap \
  --tap-if "$IFNAME" \
  --serial "file:$LOGFILE" \
  --monitor none >/dev/null 2>&1 &
QPID=$!

# --- Monitor Progress ---
START_TIME=$SECONDS
STAGE_USB=0
STAGE_RX=0
STAGE_SLAAC=0
STAGE_NDP=0
STAGE_PING=0

while (( SECONDS - START_TIME < TEST_TIMEOUT_S )); do
    # Check Guest Logs
    if [[ $STAGE_USB -eq 0 ]] && grep -q "usb-net: bound dev" "$LOGFILE"; then
        pass "Stage 1: Guest USB RNDIS bound"
        STAGE_USB=1
    fi

    # Check for RA reception (either debug counter or SLAAC success implies it)
    if [[ $STAGE_RX -eq 0 ]]; then
        if grep -q "ipv6dbg.*rx_icmpv6_ra" "$LOGFILE"; then
            pass "Stage 2: Guest received RA (RX path working)"
            STAGE_RX=1
        elif grep -q "ipv6: slaac global=" "$LOGFILE"; then
            pass "Stage 2: Guest received RA (Implied by SLAAC)"
            STAGE_RX=1
        fi
    fi

    if [[ $STAGE_SLAAC -eq 0 ]] && grep -q "ipv6: slaac global=" "$LOGFILE"; then
        pass "Stage 3: Guest configured SLAAC address"
        STAGE_SLAAC=1

        # Extract Guest IP
        GUEST_IP=$(grep "ipv6: slaac global=" "$LOGFILE" | head -n1 | sed 's/.*global=\([^ ]*\).*/\1/')
        info "Guest IP: $GUEST_IP"

        # Trigger Ping from Host to generate traffic (NS/NA + Echo)
        info "Pinging Guest from Host..."
        ping6 -c 1 -W 2 "$GUEST_IP" >/dev/null 2>&1 &
    fi

    if [[ $STAGE_NDP -eq 0 ]] && grep -q "ipv6: rx NS" "$LOGFILE"; then
        pass "Stage 4: Guest received NS (NDP working)"
        STAGE_NDP=1
    fi

    if [[ $STAGE_PING -eq 0 ]] && grep -q "ipv6: rx echo req" "$LOGFILE"; then
        pass "Stage 5: Guest received Echo Request (Ping working)"
        STAGE_PING=1
        break
    fi
    
    sleep 0.5
done

# --- Post-Mortem Analysis ---
info "Stopping Guest..."
kill "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true
kill "$TCPDUMP_PID" 2>/dev/null || true
wait "$TCPDUMP_PID" 2>/dev/null || true

echo "---------------------------------------------------"
info "Validation Summary:"

if [[ $STAGE_USB -eq 0 ]]; then
    fail "Stage 1 Failed: USB device did not bind."
    exit 1
fi

# Check PCAP for RS/RA if we didn't see RX in guest
if [[ $STAGE_RX -eq 0 ]]; then
    # Wait a bit for tcpdump to flush
    sleep 1
    
    if tcpdump -r "$PCAPFILE" -n 'icmp6 and ip6[40]=133' 2>/dev/null | grep -q "router solicitation"; then
        pass "Wire: Saw Guest RS"
    else
        fail "Wire: Did NOT see Guest RS (TX issue?)"
    fi
    
    if tcpdump -r "$PCAPFILE" -n 'icmp6 and ip6[40]=134' 2>/dev/null | grep -q "router advertisement"; then
        pass "Wire: Saw Host RA"
        fail "Stage 2 Failed: Host sent RA, but Guest did not receive/log it. (RX Issue)"
    else
        fail "Wire: Did NOT see Host RA (Host config issue?)"
    fi
    exit 1
fi

if [[ $STAGE_SLAAC -eq 0 ]]; then
    fail "Stage 3 Failed: Guest received RA but did not configure address."
    exit 1
fi

# Check PCAP for NS/NA if we didn't see NDP success
if [[ $STAGE_NDP -eq 0 ]]; then
    # Wait a bit for tcpdump to flush
    sleep 1
    
    if tcpdump -r "$PCAPFILE" -n 'icmp6 and ip6[40]=135' 2>/dev/null | grep -q "neighbor solicitation"; then
        pass "Wire: Saw Guest NS"
    else
        fail "Wire: Did NOT see Guest NS"
    fi

    if tcpdump -r "$PCAPFILE" -n 'icmp6 and ip6[40]=136' 2>/dev/null | grep -q "neighbor advertisement"; then
        pass "Wire: Saw Host NA"
        fail "Stage 4 Failed: Host sent NA, but Guest did not receive/log it. (RX Drop Issue)"
    else
        fail "Wire: Did NOT see Host NA (Host did not reply?)"
    fi
    exit 1
fi

if [[ $STAGE_PING -eq 0 ]]; then
    fail "Stage 5 Failed: NDP worked, but Ping did not PASS."
    exit 1
fi

pass "ALL SYSTEMS GO. Network stack is fully operational."
exit 0
