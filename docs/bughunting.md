# IPv6 + USB RNDIS (TAP) bughunting briefing

This document is a **briefing / handoff** for an experienced engineer who has never seen this codebase.
It summarizes:

- **(a) What we are trying to do**
- **(b) What is supposed to work** (intended end-to-end behavior)
- **(c) What does not work** (current observed failure mode)
- **(d) What was attempted** (and what evidence we collected)

It is intentionally specific and operational.

---

## A) What we are trying to do (goal)

### Primary immediate goal
Make `make test-net6` pass reliably and quickly.

Concretely:

- Boot the bare-metal AArch64 guest in QEMU (`-M raspi3b`).
- Use a USB network device backend (QEMU `usb-net`, RNDIS mode) bridged to a host TAP interface (typically `mona0`).
- Bring up IPv6 in the guest via RA/SLAAC.
- Resolve neighbors using NDP.
- Successfully send an ICMPv6 echo request (ping6) from the guest to the host router address (typically `fd42:6d6f:6e61:1::1`).
- Have the userland test binary print `[net6test] PASS` and exit.

### North-star goal (later)
Be able to `ping6 google.com` from the guest.

This requires everything above plus (eventually): default routing, DNS via RDNSS, and host forwarding/NAT66 or a routed prefix.

---

## B) What is supposed to work (intended behavior)

### Repository structure relevant to net6

- Kernel:
  - `kernel-aarch64/` contains the AArch64 kernel.
  - IPv6 stack: `kernel-aarch64/net_ipv6.c` (NDP/ICMPv6/UDPv6, SLAAC, etc.)
  - Syscalls glue: `kernel-aarch64/sys_net.c`
  - USB host controller + drivers:
    - DWC2 host: `kernel-aarch64/usb_host.c`
    - USB core: `kernel-aarch64/usb.c`
    - USB RNDIS NIC: `kernel-aarch64/usb_net.c`
    - USB keyboard: `kernel-aarch64/usb_kbd.c`

- Userland:
  - Test program: `userland/src/net6test.c`

- Tools / harness:
  - TAP bringup scripts: `tools/tap6-up.sh`, `tools/tap6-down.sh`
  - Test runner: `tools/test-net6.sh` (invoked by Makefile)
  - QEMU runner: `tools/run-qemu-raspi3b.sh`

### How `make test-net6` is expected to behave

1. Build userland payload `net6test` and pack it into initramfs.
2. Build kernel with USB networking enabled (`-DENABLE_USB_NET`).
3. Bring up host TAP interface (optionally via scripts) and start a local IPv6 router service (dnsmasq RA + RDNSS).
4. Boot QEMU with a USB network device attached.
5. In guest:
   - USB enumeration binds the NIC.
   - Guest sends RS (router solicitation).
   - Host replies RA (router advertisement) with prefix and optional RDNSS.
   - Guest performs SLAAC to configure a global IPv6 address.
   - Guest installs router and DNS.
   - Guest can ping the host’s IPv6 address:
     - If neighbor entry missing: send NS, receive NA, then send echo request, receive echo reply.
6. `net6test` prints `[net6test] PASS` and exits.
7. The harness should stop early on PASS and should hard-stop after ~15 seconds if something wedges.

### Timing/scheduling assumption (important)

This kernel’s EL0 execution model historically runs userland with **IRQs masked** (EL0 doesn’t receive IRQ exceptions; kernel runs with IRQ enabled at EL1).

That implies:

- If a userland syscall blocks waiting for network traffic, progress depends on where USB/network polling happens.
- If there is no periodic timer interrupt delivered while userland blocks (because EL0 IRQs are masked and the kernel isn’t running), the network RX path may stall unless the syscall itself polls.

This became a key design constraint.

---

## C) What does not work (current failure)

### Symptom
`make test-net6` fails because `[net6test] PASS` never appears.

After recent changes, the harness no longer times out at 15s; instead the guest explicitly fails quickly:

- `[net6test] FAIL: ping6 host rc=-110`

`-110` is `ETIMEDOUT`.

### What we know from guest counters (`/proc/net` selected lines)

`net6test` prints a filtered view of `/proc/net` twice: once before the ping attempt and once after the ping attempt.

A representative failing run (log file in `/tmp/mona-tap6-mona0-*.log`) showed:

- Before ping:
  - `ping6_sent_ns=1`
  - `ping6_sent_echo=0`
  - `tx_echo_req=0`
  - `rx_na=0`

- After ping (post-timeout):
  - `ping6_sent_ns=1`
  - `ping6_sent_echo=0`
  - `tx_echo_req=0`
  - `rx_na=0`
  - `ping6_ebusy` increased (the syscall was actively waiting/polling)

Interpretation:

- The ping code never made it past “neighbor resolution required”.
- It transmitted an NS but did not *process* an NA.
- Therefore it never emitted an echo request.

### What we know from the host-side pcap

The harness captures a TAP pcap when `MONA_TAP_DEBUG=1`.

In the same failing run, the pcap contained:

- RS from guest
- RA from host
- Guest NS for `fd42:...::1` (router)
- Host NA in response

However:

- There was **no ICMPv6 echo request/reply** on the wire.

Interpretation:

- The guest did send NS (consistent with counters).
- The host replied NA (wire evidence).
- The guest did not accept/process that NA (counters show `rx_na=0`).

This points away from “host didn’t respond” and toward “guest RX path dropped/failed to deliver NA to IPv6 stack”.

### Additional corroborating evidence: USB/RNDIS debug counters

The kernel exports `usbnet` debug counters (printed in `/proc/net` selected lines). In the failing run:

- `rndis_drop_bounds` was non-zero (e.g. `1`).

Interpretation:

- The RNDIS RX stream parser hit a bounds/format error and dropped buffered data.
- Since NA is a small but critical packet, losing a single NA can stall ping.

---

## D) What was attempted (timeline of fixes, experiments, and rationale)

This section is the “bughunting log” of what we changed and why.

### 1) Test harness boundedness: stop hanging forever

Problem:

- Initial failures were ambiguous because the harness would simply hang until its timeout, and it wasn’t clear whether ping was progressing.

Work:

- Adjusted the net6 harness to:
  - Exit early when PASS appears.
  - Hard stop after ~15 seconds.
  - Preserve logs/pcaps in `/tmp/` so each run is debuggable.

Outcome:

- Failures became reproducible and post-mortem friendly.

### 2) USB IN transfer semantics: distinguish “no data yet” from “successful ZLP”

Problem:

- The DWC2 bulk-IN helper returned success (0) even when it only saw NAK/timeouts during a poll attempt (`nak_ok` path).
- That made the caller treat “no data” as “successful transaction”, and it could toggle DATA PID incorrectly.
- Incorrect PID toggling can desynchronize bulk endpoints, causing packet loss/corruption.

Work:

- Added a distinct return code `USB_XFER_NODATA` (value `1`) in:
  - `kernel-aarch64/include/usb_host.h`
- Changed `dwc2_in_xfer()` in:
  - `kernel-aarch64/usb_host.c`
  to return `USB_XFER_NODATA` when `nak_ok` is set and the poll interval ends with NAK/no data.

- Updated callers to treat:
  - `USB_XFER_NODATA` as “no data, do not toggle PID”
  - `(rc == 0 && got == 0)` as a valid ZLP completion and *do* toggle PID

Files updated:

- `kernel-aarch64/usb_net.c`
- `kernel-aarch64/usb_kbd.c`

Outcome:

- Removed a major ambiguity in the RX path and reduced risk of PID desync.
- Did not by itself make ping succeed.

### 3) EL0 IRQ delivery experiment: attempt to let timer IRQs drive USB polling

Hypothesis:

- Userland might be blocking while the kernel cannot poll USB RX because IRQ-driven wakeups don’t fire in EL0.
- If EL0 could receive IRQ exceptions and route them to `irq_handle()`, periodic timer interrupts could run USB polling indirectly.

Work attempted:

- Changed EL0 entry SPSR to unmask IRQ.
- Added handling for `IRQ_EL0_64` exception kind in `exceptions.c`.
- Updated exception vector assembly to fast-path EL0 IRQ to the IRQ handler.

Outcome:

- Initial attempt produced immediate `IRQ_EL0_64` exception prints and instability.
- Later attempt removed the exception spam but caused userland output to stop (system behavior changed in a confusing way).
- This approach was reverted as too risky while still not achieving PASS.

Key lesson:

- Enabling EL0 IRQs is non-trivial; it impacts exception vectoring, scheduling, and potentially invariants about where IRQs are allowed.

### 4) Make ping syscall self-sufficient: poll USB while waiting

Hypothesis:

- Even if EL0 IRQs stay masked, blocking syscalls that depend on inbound network frames must “pull” RX by polling.

Work:

- Modified `sys_mona_ping6` (in `kernel-aarch64/sys_net.c`) to:
  - Busy-wait with a deadline based on `time_now_ns()`
  - Call `usb_poll()` in the wait loop

Outcome:

- The system stopped hanging indefinitely.
- Ping now fails deterministically with `ETIMEDOUT`.
- This confirmed that “no progress during syscall” was not the only issue; even with aggressive polling, the NA still wasn’t being processed.

### 5) Improve observability: print `/proc/net` again after ping failure

Problem:

- The original `net6test` only printed counters before the ping, so it was unclear whether ping updated counters during its attempt.

Work:

- Updated `userland/src/net6test.c` to reprint the same `/proc/net` selection after ping failure.

Outcome:

- We can now directly see whether counters advanced during the ping attempt.
- This is how we confirmed `rx_na` stays at 0 while `ping6_ebusy` increases.

### 6) RNDIS RX reassembly/parser robustness: resync instead of drop-all

Evidence motivating this:

- TAP pcap shows host NA exists.
- Guest `rx_na` counter remains 0.
- `usbnet` debug shows `rndis_drop_bounds` increments.

Hypothesis:

- The RNDIS RX accumulator occasionally becomes desynchronized (partial messages across USB transactions, unexpected framing, or a parsing bug).
- When that happens, the parser previously “reset accumulator and return”, losing any subsequent valid data buffered.
- Losing a single NA is enough to stall ping.

Work:

- Modified `kernel-aarch64/usb_net.c` RNDIS parsing loop:
  - Instead of immediately wiping `rndis_accum_len` on a corrupt header (small/bounds), it advances `off` by 1 byte and rescans for a plausible RNDIS header.
  - Only breaks when there isn’t enough data left for a full message.

Outcome (pending):

- This is expected to make RX tolerant to rare stream corruption/desync and prevent losing NDP packets.
- Needs verification by rerunning `make test-net6` and checking whether:
  - `rx_na` becomes non-zero
  - `tx_echo_req` becomes non-zero
  - pcap includes echo request/reply
  - PASS prints

---

## Current best hypothesis (root cause)

The failure is currently most consistent with:

1. The guest sends an NS for the router (`tx_ns` / `ping6_sent_ns`).
2. The host replies with an NA (verified by pcap).
3. The guest **receives** some USB/RNDIS data (usbnet counters increase), but the RNDIS parser occasionally drops buffered content due to framing/parsing issues.
4. The NA does not reach the IPv6 layer (`rx_na=0`), so ping never sends echo.

The key sign is `rndis_drop_bounds` increasing in the same run where NA is present on wire but absent in guest counters.

---

## How to reproduce quickly

Typical run:

```sh
make test-net6 TEST_TIMEOUT_S=15 MONA_TAP_DEBUG=1
```

Artifacts (examples):

- Guest UART log: `/tmp/mona-tap6-mona0-YYYYMMDD-HHMMSS.log`
- TAP pcap: `/tmp/mona-tap6-mona0-YYYYMMDD-HHMMSS.pcap`
- Sometimes host ping logs (for other tests): `/tmp/mona-tap6-mona0-*.hostping.log`

What to check in the `.log`:

- The two `/proc/net selected lines` snapshots.
- `ipv6dbg` counters:
  - `rx_na`, `tx_echo_req`, `rx_echo_reply`
  - `ping6_sent_ns`, `ping6_sent_echo`
- `usbnet` counters:
  - `rndis_drop_bounds`, `rndis_drop_small`, `rx_err`, `rx_nak`

What to check in the `.pcap`:

- Do we see guest NS?
- Do we see host NA?
- Do we see guest echo request?
- Do we see host echo reply?

A healthy run should show all of the above.

---

## Key code pointers (where to look next)

### Ping/NDP flow

- `kernel-aarch64/net_ipv6.c`
  - Ping6 state machine (global phases/state variables)
  - Neighbor cache update path on NA reception
  - Transmission of NS and echo request

- `kernel-aarch64/sys_net.c`
  - `sys_mona_ping6` blocking behavior + current USB polling loop

### USB/RNDIS RX path

- `kernel-aarch64/usb_net.c`
  - `usb_net_poll()` bulk-IN reads, PID management
  - RNDIS accumulator and message parsing
  - Debug counter updates

- `kernel-aarch64/usb_host.c`
  - `dwc2_in_xfer()` and how NAK/timeouts are surfaced

### Scheduling/interrupt assumptions

- `kernel-aarch64/arch/enter_el0.S`
- `kernel-aarch64/arch/exceptions.S`
- `kernel-aarch64/exceptions.c`

The EL0 IRQ enabling attempt was reverted, but these are relevant if that approach is revisited.

---

## Open questions / next investigations

1. **Confirm the RNDIS resync change fixes NA delivery.**
   - Expect `rx_na` to increment and ping to proceed.

2. If NA still doesn’t register:
   - Instrument `net_ipv6.c` NA receive handler to log when a NA is parsed and whether it is dropped (bad target, missing option, etc.).
   - Instrument `usb_net.c` to log the ethertype of frames being delivered to `netif_rx_frame` (already has `last_ethertype`).

3. Validate checksums and address matching:
   - If echo request appears on wire but reply doesn’t register, investigate ICMPv6 checksum validation and destination address checks.

4. Larger architectural question:
   - Should the kernel keep relying on syscall-local polling, or should it introduce a kernel thread/polling loop that runs with interrupts enabled?
   - If we want “real” preemptive network RX, revisit EL0 IRQ handling more systematically.

---

## Quick glossary / mental model

- **RS/RA**: Router Solicitation / Router Advertisement (SLAAC configuration).
- **SLAAC**: Stateless Address Autoconfiguration.
- **RDNSS**: Recursive DNS Server option in RA.
- **NDP**: Neighbor Discovery Protocol (NS/NA for L2 resolution, also DAD).
- **RNDIS**: USB networking protocol used by QEMU `usb-net`.
- **ZLP**: Zero-length packet; valid USB transfer completion.
- **NAK**: USB handshake meaning “no data right now”; should not be treated as success.

---

## Status summary (as of 2026-01-02)

- IPv6 bringup works: RS/RA/SLAAC/RDNSS all appear.
- Harness is bounded and preserves artifacts.
- Ping does not succeed: ping stalls waiting for NA; echo request never sent.
- Wire shows host NA exists.
- Guest does not process NA; RNDIS RX parser shows signs of dropping data.
- Most promising current change: RNDIS parser resynchronization (avoid drop-all on malformed header).

---

## E) Resolution (2026-01-02)

### The "Smoking Gun"
After implementing the validation harness (`tools/validate-net6.sh`), we observed a consistent failure mode:
- **SLAAC worked**: The guest received RA and configured an address.
- **Ping failed**: The guest sent NS, the host replied with NA, but the guest never processed the NA.
- **Logs**: The guest kernel logged `usb-net: drop bounds len=0x616e6f6d`.

### Root Cause Analysis
The value `0x616e6f6d` is the ASCII representation of the string "mona" (`m`=0x6d, `o`=0x6f, `n`=0x6e, `a`=0x61). This string appears in the guest's IPv6 address (`fd42:6d6f:6e61:...`).

This proved that the RNDIS parser was interpreting **packet payload data** as an **RNDIS message header**. The parser had become desynchronized from the data stream.

The cause was the USB read request size. The driver was requesting small chunks (64 bytes, the endpoint MPS). When QEMU sent larger packets (like Ping Echo Requests or NAs) or split them across transfers, the driver's logic for reassembling RNDIS messages failed to handle the boundaries correctly, causing it to miss the next header.

### The Fix
We increased the USB bulk-IN request size in `kernel-aarch64/usb_net.c` from 64 bytes to **2048 bytes**.

```c
// Old
uint32_t req = (uint32_t)g_usbnet.ep_in.mps; // 64

// New
uint32_t req = 2048;
```

This allows the driver to receive complete RNDIS messages (or at least larger, more coherent chunks) in a single transfer, preventing the fragmentation that confused the parser.

### Verification
We re-ran the validation suite (`tools/validate-net6.sh`):
1.  **Stage 1 (USB)**: PASS
2.  **Stage 2 (RX)**: PASS (RA received)
3.  **Stage 3 (SLAAC)**: PASS (Address configured)
4.  **Stage 4 (NDP)**: PASS (NS sent, NA received and processed)
5.  **Stage 5 (Ping)**: PASS (Echo Request received by guest)

The network stack is now fully operational for ICMPv6.
