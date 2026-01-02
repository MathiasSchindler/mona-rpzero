# IPv6-only networking: implementation plan (QEMU → Raspberry Pi Zero 2 W)

This document is a step-by-step plan to implement an IPv6-only network stack in this repo.

Scope assumptions (adjust if you want something different):
- **IPv6-only**: no IPv4, no dual-stack, no NAT64.
- **Link layer**: Ethernet framing (including USB Ethernet class devices).
- **QEMU bring-up**: use QEMU’s `-device usb-net,netdev=...` path (enabled via `make run USB_NET=1`).
- **Real hardware bring-up**: start with a **USB Ethernet dongle** (CDC-ECM preferred; optionally RNDIS). WiFi on the Zero 2 W (CYW434xx) is a much bigger project and can come later.
- **“Full stack” meaning** (pragmatic): IPv6 core + ICMPv6 + UDP + TCP + a minimal sockets-like API and a few userland tools (ping, ifconfig/ip, maybe nc/curl later).

If you want a different definition of “full” (e.g., mandatory DHCPv6, MLDv2, TLS, HTTP client), we can tweak milestones.

---

## Phase 0 — Preflight: decide the external interface path

1) **Confirm QEMU NIC strategy**
   - Enable USB NIC in QEMU: run with `USB_NET=1`.
  - The QEMU wrapper supports two backends:
    - **user** (default): `-netdev user,id=net0,ipv6=on -device usb-net,netdev=net0`
    - **tap**: `-netdev tap,... -device usb-net,...` (see below)
  - Select backend via `USB_NET_BACKEND=user|tap`.
   - Reality check: user-mode networking is great for “it works” smoke tests, but may be limited for advanced IPv6 scenarios.

2) **Plan for “serious” IPv6 testing** (optional but recommended early)
   - Add a QEMU option for **TAP** networking in addition to `-netdev user`.
   - TAP will let you:
     - run Router Advertisements (RA) from the host,
     - test SLAAC + link-local + neighbor discovery in a realistic L2 environment,
     - connect to your LAN / a test namespace.

Quick usage:
- User-mode IPv6 (simple): `make run USB_NET=1 USB_NET_BACKEND=user`
- TAP (realistic L2): `make run USB_NET=1 USB_NET_BACKEND=tap TAP_IF=mona0`

Implementation notes:
- The Makefile passes `--usb-net-backend` / `--tap-if` into the QEMU launcher.
- The launcher logic lives in `tools/run-qemu-raspi3b.sh`.

Deliverable for Phase 0:
- A clear decision: start with `-netdev user` for initial bring-up, then add TAP for real testing.

---

## Phase 1 — Kernel architecture scaffolding (net core)

3) **Introduce a small, driver-agnostic network core**
   - Create a `net/` subsystem with these conceptual layers:
     - `netif`: interface abstraction (send frame, receive frame callback, link state, MTU, MAC)
     - `ether`: Ethernet framing + demux by EtherType
     - `ipv6`: IPv6 input/output, routing decision, fragmentation policy
     - `icmpv6`: echo + ND messages
     - `udp` and `tcp`
     - `sock` layer: syscalls and per-process socket objects

4) **Define the boundary between driver and stack**
   - Driver provides:
     - MAC address, link status, MTU
     - `tx(frame)` async
     - pushes received frames into the stack
   - Stack provides:
     - Ethernet demux
     - ARP is not needed (IPv6 uses NDP)

5) **Add basic packet buffers**
   - Implement a simple `pbuf`/mbuf-like structure:
     - pointer + length, headroom for pushing headers, reference counting if needed
     - optional scatter/gather later
   - Keep it simple at first: single contiguous buffers.

Deliverable for Phase 1:
- Kernel can accept a “fake netif” and send/receive Ethernet frames through the stack API.

---

## Phase 2 — QEMU USB NIC driver (Ethernet over USB)

6) **Identify what `-device usb-net` expects**
   - Treat it as a USB network adapter presenting an Ethernet-like interface.
   - Implement a USB class driver that can:
     - enumerate the device
     - find the data interfaces/endpoints
     - send/receive Ethernet frames

7) **Implement the minimal USB net driver**
   - Start with one device model path; expand once proven.
   - Typical candidates:
     - **CDC-ECM** (USB Communications Device Class, Ethernet Control Model)
     - **CDC-NCM** (more complex; defer)
     - **RNDIS** (common but more protocol overhead)

8) **Plumb driver RX into the net core**
   - Receive path:
     - USB interrupt/bulk completion → enqueue frames → net core ethernet input
   - Transmit path:
     - net core ethernet output → USB bulk OUT

9) **Basic driver diagnostics**
   - Add a kernel command or log hooks to dump:
     - USB descriptors for the device
     - negotiated endpoints
     - rx/tx counters and drop reasons

Deliverable for Phase 2:
- In QEMU with `USB_NET=1`, kernel sees a net interface and can tx/rx raw Ethernet frames.

---

## Phase 3 — IPv6 core (link-local first)

10) **Implement IPv6 parsing and output**
   - Validate version, payload length, next header, hop limit.
   - Handle extension headers minimally:
     - parse/skip Hop-by-Hop and Routing headers if present (or drop for now)
   - Initially: no fragmentation/reassembly (drop fragments; add later).

11) **Assign a link-local address**
   - IPv6 requires a link-local address for NDP.
   - Compute interface ID:
     - easiest: stable EUI-64 from MAC (OK for now)
     - later: stable privacy addresses

12) **Implement ICMPv6 echo**
   - `ping6` requires:
     - ICMPv6 checksum
     - echo request/reply handling

Deliverable for Phase 3:
- You can `ping6` a link-local peer in a TAP-based setup, or at least see ICMPv6 traffic in logs.

---

## Phase 4 — Neighbor Discovery (required for “real” IPv6)

13) **Neighbor cache + NDP messages**
   - Implement Neighbor Solicitation (NS) and Neighbor Advertisement (NA)
   - Maintain a neighbor cache:
     - state machine can be simplified initially (INCOMPLETE/REACHABLE/STALE)
     - timers can be coarse at first

14) **DAD (Duplicate Address Detection)**
   - For safety and correctness, implement basic DAD for the addresses you assign.

15) **Router Solicitation / Router Advertisement**
   - Implement RS (send) and RA (receive).
   - On RA receive:
     - install default route (router)
     - record prefix information
     - perform SLAAC address configuration (global address)

Deliverable for Phase 4:
- With a router/host providing RA (host radvd in TAP setup), your kernel gets a global IPv6 address + default route.

---

## Phase 5 — UDP + a first user-visible tool

16) **Implement UDP sockets**
   - Add kernel objects for UDP “sockets” and a minimal API:
     - `socket(AF_INET6, SOCK_DGRAM, 0)`
     - `bind`, `sendto`, `recvfrom`
   - Minimal recv queue, blocking semantics, and timeouts.

17) **Userland tools for validation**
   - Add:
     - `ping6`
     - `ip`/`ifconfig`-like tool to print addresses, routes, neighbors
     - optionally `udp-echo` client/server for functional tests

Deliverable for Phase 5:
- You can run a UDP echo client/server over IPv6.

---

## Phase 6 — TCP (enough for practical use)

18) **Implement TCP basics**
   - State machine: LISTEN, SYN-SENT, SYN-RECEIVED, ESTABLISHED, FIN-WAIT, etc.
   - Retransmission (RTO), basic congestion avoidance (start with something simple).
   - Receive reassembly for out-of-order segments.

19) **Expose TCP through sockets**
   - `socket(AF_INET6, SOCK_STREAM, 0)`
   - `connect`, `listen`, `accept`, `send`, `recv`, `shutdown`

20) **Userland tool for TCP**
   - Add a minimal `nc` (netcat) or `http-get` to validate TCP end-to-end.

Deliverable for Phase 6:
- You can make a TCP connection to a host service (in TAP setup) and transfer data.

---

## Phase 7 — Portability to real Raspberry Pi Zero 2 W

21) **Keep the network core driver-agnostic**
   - The USB driver is just one `netif` backend.
   - The IPv6/UDP/TCP layers should not know/care whether packets come from USB, WiFi, etc.

22) **Real hardware strategy (recommended order)**

**22a) USB Ethernet dongle first (fastest path)**
- Use the existing USB host work and extend it to support USB Ethernet class devices.
- Target CDC-ECM first because it’s relatively straightforward and “Ethernet-like”.

**22b) Native WiFi later (large project)**
- Pi Zero 2 W WiFi is typically Broadcom/Cypress CYW434xx over SDIO, with firmware.
- That involves:
  - SDIO controller driver
  - firmware loading
  - 802.11 MAC/PHY integration (or a very small subset)
  - WPA2/RSN if you want secure networks
- Treat this as a separate milestone after the wired stack is solid.

23) **DTB + hardware discovery alignment**
- Ensure the same net core works regardless of DTB differences.
- For real hardware, parse DTB for:
  - USB controller base address/IRQs
  - SDIO/WiFi nodes (later)

Deliverable for Phase 7:
- Same IPv6 stack runs on QEMU and on real hardware with a USB Ethernet dongle.

---

## Phase 8 — Test strategy (so we don’t regress)

24) **Add kernel-side test hooks**
- Counters: rx/tx packets, checksum errors, nd cache entries.
- Optional: a “pcap dump” facility (write a pcap stream to UART or semihosting in QEMU).

25) **Extend `kinit` selftests**
- Start small and deterministic:
  - verify that the USB net device enumerates (in QEMU with `USB_NET=1`)
  - verify link-local address exists
  - verify NDP cache updates after a synthetic NS/NA exchange (if you build an internal test harness)
- Avoid tests that depend on external host configuration unless you gate them behind a flag.

26) **Host-side integration tests (optional)**
- Add scripts to set up a TAP device + network namespace + radvd.
- Then run QEMU attached to TAP and validate:
  - SLAAC obtains address
  - `ping6` host
  - UDP/TCP echo

---

## Suggested milestone checklist

- M1: QEMU enumerates USB net device; raw Ethernet rx/tx works.
- M2: IPv6 link-local + ICMPv6 echo works.
- M3: NDP + SLAAC via RA works (prefer TAP).
- M4: UDP sockets + udp-echo works.
- M5: TCP sockets + basic client works.
- M6: Same stack works on real Pi Zero 2 W using a USB Ethernet dongle.
- M7: (Optional) WiFi support as a separate project.

---

## Notes / decisions to make early

- **Fragmentation**: often safe to defer initially; many modern paths avoid fragmentation via PMTU discovery.
- **Extension headers**: you can start by dropping packets with unknown extension headers.
- **Security**: a minimal stack will be fragile; don’t expose it to hostile networks early.
- **Performance**: focus on correctness first; optimize copy counts and checksum offload later.

---

## Host OS notes (Linux vs macOS with Homebrew QEMU)

This matters mainly for the **Phase 0 backend choice**.

### Recommended workflow

- **Cross-platform (Linux + macOS):** use `USB_NET_BACKEND=user` for day-to-day.
  - It’s easy to run everywhere and good for driver + stack development.
  - It’s *less realistic* for L2 behaviors (RA/NDP edge cases), so don’t treat it as your final validation.
- **Realistic IPv6 L2 testing:** prefer `USB_NET_BACKEND=tap` on **Linux**.

### Linux

- TAP is first-class on Linux.
- You can build a proper IPv6 testbed:
  - provide Router Advertisements (RA) from a host namespace,
  - validate SLAAC/DAD/NDP with real Ethernet frames,
  - optionally bridge to a lab LAN.

### macOS (Homebrew QEMU)

- `USB_NET_BACKEND=user` is typically the most reliable option.
- `USB_NET_BACKEND=tap` may work, but it’s not “stock macOS”:
  - you need a TAP device provider (commonly `tuntaposx`),
  - setup differs from Linux, and it’s easy to end up with permissions/driver issues.

Practical rule of thumb:
- Do quick iteration on either host with `USB_NET_BACKEND=user`.
- Do the serious RA/SLAAC/NDP validation on a Linux host using TAP.

If you want, we can add a dedicated helper script for Linux TAP setup (bridge/netns + RA) and keep macOS documented as “user-mode only”.

If you tell me your preferred host setup (Linux with TAP/bridge available, or you want to stick to `-netdev user`), I can add a concrete “QEMU IPv6 testbed” section with exact commands and expected outputs.
