# Network Stack Validation Plan

This document outlines a structured testing scenario to validate the underlying assumptions of the `mona-rpzero` network stack. The goal is to isolate failures to specific layers (USB, Ethernet, IPv6, NDP) and distinguish between guest-side (OS) and host-side (QEMU/TAP) issues.

## Testing Architecture

The test environment consists of:
1.  **Host (Linux):** Runs QEMU, a TAP interface (`mona0`), and `dnsmasq` (providing IPv6 Router Advertisements).
2.  **Transport (QEMU):** Emulates a USB connection carrying RNDIS packets.
3.  **Guest (Mona OS):** Runs the USB host stack, RNDIS driver, and IPv6 stack.

## Validation Stages

We validate connectivity bottom-up. Each stage relies on the previous one.

### Stage 1: Host Environment & Transport
**Assumption:** The host TAP interface is correctly configured, and QEMU is successfully passing USB packets to the guest.

*   **Host Check:** Verify `mona0` exists, is UP, and has an IPv6 address (`fd42:6d6f:6e61:1::1`).
*   **Host Check:** Verify `dnsmasq` is running and bound to `mona0`.
*   **Guest Check:** Verify the USB DWC2 controller initializes.
*   **Guest Check:** Verify the USB enumeration finds the RNDIS device (VID 0525:a4a2).
*   **Guest Check:** Verify `usbnet` binds and transitions to `USBNET_MODE_RNDIS`.

### Stage 2: Link Layer (Ethernet/RNDIS)
**Assumption:** The guest can receive raw Ethernet frames from the host, and the RNDIS protocol is correctly decapsulating them.

*   **Action:** Host sends periodic Router Advertisements (multicast `33:33:00:00:00:01`).
*   **Guest Check:** `usbnet` RX counters (`rx_usb_xfers`, `rx_rndis_ok`) must increment.
*   **Guest Check:** `usbnet` error counters (`rx_errors`, `rx_rndis_drop_*`) should remain low/zero.
*   **Guest Check:** `ipv6dbg` counters (`rx_ipv6_packets`) must increment (proving RNDIS decapsulation worked).

### Stage 3: IPv6 Configuration (SLAAC)
**Assumption:** The guest can send packets (RS) and process specific configuration packets (RA) to configure itself.

*   **Action:** Guest sends Router Solicitation (RS).
*   **Host Check (pcap):** Verify RS packet appears on `mona0`.
*   **Action:** Host responds with Router Advertisement (RA).
*   **Guest Check:** `ipv6dbg` counter `rx_icmpv6_ra` increments.
*   **Guest Check:** Guest configures a global address (e.g., `fd42:6d6f:6e61:1:xxxx:xxxx:xxxx:xxxx`).
*   **Guest Check:** Guest installs a default router (`fe80::...`).

### Stage 4: Neighbor Discovery (NDP)
**Assumption:** The guest can resolve the link-layer address of the gateway to send unicast traffic.

*   **Action:** Guest attempts to ping the host (`fd42:6d6f:6e61:1::1`).
*   **Guest Check:** Guest sends Neighbor Solicitation (NS) for the gateway.
*   **Host Check (pcap):** Verify NS packet appears on `mona0`.
*   **Action:** Host responds with Neighbor Advertisement (NA).
*   **Guest Check:** `ipv6dbg` counter `rx_icmpv6_na` increments.
    *   *Failure here indicates RX drop or parsing failure of the NA packet.*
*   **Guest Check:** Guest Neighbor Cache updates (transition from incomplete to reachable).

### Stage 5: ICMPv6 Connectivity
**Assumption:** Bidirectional unicast traffic works.

*   **Action:** Guest sends Echo Request.
*   **Host Check (pcap):** Verify Echo Request on `mona0`.
*   **Action:** Host sends Echo Reply.
*   **Guest Check:** `ipv6dbg` counter `rx_icmpv6_echo_reply` increments.
*   **Guest Check:** Ping syscall returns success.

## Execution Plan

To execute this validation, we will use a modified test harness that:
1.  Starts the environment (`tap6-up.sh`).
2.  Starts `tcpdump` on the host to validate Host Checks.
3.  Runs QEMU with the guest payload.
4.  Correlates Guest UART logs with Host PCAP data in real-time (or post-mortem).

### Success Criteria
The system is working if we observe the full chain:
`USB Enum -> RX Packets -> TX RS -> RX RA -> SLAAC Config -> TX NS -> RX NA -> TX Echo -> RX Reply -> PASS`

### Failure Analysis
*   **No RX Packets:** USB/RNDIS transport issue.
*   **TX RS but no RA:** Host configuration issue (dnsmasq).
*   **RX RA but no Config:** Guest RA parsing issue.
*   **TX NS but no NA:** Host issue (not responding to NS) or Guest TX issue (malformed NS).
*   **NA on wire but no RX NA:** Guest RX path issue (RNDIS parser dropping small packets). **<-- Current Suspect**
