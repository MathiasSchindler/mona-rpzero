# Tooling status (syscall-only userland)

This file tracks the status of a planned **syscall-only** tool suite (inspired by the monacc project list you shared) for this repo.

## Status legend

- **Done**: usable and “feature-complete enough” for this project’s goals
- **Partial**: exists, but missing common flags/behaviors
- **Planned**: not implemented yet

## Counting method for “Flags/Features (#)”

A single integer counting:

- each supported CLI flag (e.g. `-n`, `-a`), plus
- each major non-flag capability (e.g. “supports stdin”, “supports multiple files”, “supports pipelines”)

This is intentionally coarse; it’s a progress indicator, not a spec.

## Tools (monacc list)

| Tool | Status | Flags/Features (#) |
|---|---:|---:|
| dmesg | Done | 3 |
| kinit | Done | 1 |
| pstree | Partial | 2 |
| time | Partial | 2 |
| awk | Partial | 3 |
| ln | Partial | 2 |
| pwd | Partial | 1 |
| basename | Partial | 2 |
| du | Partial | 5 |
| ls | Partial | 5 |
| readelf | Partial | 2 |
| touch | Partial | 2 |
| echo | Partial | 1 |
| readlink | Partial | 1 |
| tr | Partial | 5 |
| bpe | Planned | 0 |
| env | Partial | 4 |
| dirname | Partial | 3 |
| rev | Partial | 2 |
| true | Done | 1 |
| expr | Planned | 0 |
| rm | Done | 4 |
| rmdir | Done | 1 |
| uname | Partial | 1 |
| cat | Partial | 2 |
| false | Done | 1 |
| uncpio | Planned | 0 |
| chmod | Partial | 2 |
| find | Partial | 5 |
| mkdir | Partial | 3 |
| sed | Partial | 4 |
| uniq | Partial | 4 |
| chown | Planned | 0 |
| free | Done | 2 |
| more | Planned | 0 |
| seq | Partial | 3 |
| uptime | Done | 2 |
| mount | Planned | 0 |
| sh | Partial | 4 |
| watch | Planned | 0 |
| clear | Partial | 1 |
| mv | Partial | 3 |
| sha256 | Planned | 0 |
| wc | Partial | 4 |
| cmp | Planned | 0 |
| grep | Partial | 7 |
| sleep | Done | 2 |
| wget6 | Planned | 0 |
| col | Planned | 0 |
| gunzip | Planned | 0 |
| nl | Planned | 0 |
| which | Partial | 3 |
| column | Planned | 0 |
| gzip | Planned | 0 |
| sort | Partial | 4 |
| who | Partial | 1 |
| cp | Partial | 4 |
| head | Partial | 4 |
| stat | Partial | 2 |
| whoami | Partial | 1 |
| cpio | Planned | 0 |
| hexdump | Partial | 2 |
| objdump | Partial | 2 |
| strings | Planned | 0 |
| cut | Partial | 5 |
| od | Partial | 7 |
| tail | Partial | 4 |
| date | Done | 1 |
| hostname | Planned | 0 |
| paste | Planned | 0 |
| tar | Planned | 0 |
| xargs | Partial | 3 |
| df | Planned | 0 |
| id | Partial | 3 |
| xxd | Partial | 2 |
| diff | Partial | 5 |
| init | Partial | 1 |
| printf | Partial | 4 |
| tee | Partial | 3 |
| yes | Done | 1 |
| kill | Done | 3 |
| ps | Done | 2 |
| test | Partial | 5 |

Notes on current “Partial” implementations:

- `ls`: lists `/` only; no args/flags.
- `echo`: prints argv separated by spaces; no flags.
- `cat`: supports stdin→stdout and a single file argument.
- `awk`: supports `/TEXT/ {print ...}` substring match, `-F` single-char separator, and printing `$N`, `NR`, `NF`.
- `du`: prints byte totals; by default prints directory totals (post-order); flags: `-s` summary-only, `-a` include files, `-h` human-readable.
- `tr`: supports basic SET parsing (ranges like `a-z`, escapes like `\\n`/`\\t`/`\\xNN`); flags: `-d` delete, `-s` squeeze repeats, `-c` complement SET1.
- `ln`: supports hardlinks by default, and `-s` for symlinks.
- `readlink`: prints the symlink target (via `readlinkat`).
- `uname`: prints `sysname release machine`; no flags.
- `sleep`: accepts `SECONDS[.FRACTION]` and calls `nanosleep`.
- `sh`: supports interactive mode, `-c`, and a single `cmd1 | cmd2` pipeline.
- `init`: system init that starts `/bin/sh`.
- `kinit`: selftest runner for `make test`.

Time notes:

- `uptime` prints time since boot using `CLOCK_MONOTONIC`.
- `uptime` prints both `HH:MM:SS` and raw seconds.
- `date` prints a boot-relative “realtime” based on `CLOCK_REALTIME` (currently the kernel returns the same clock as monotonic; no RTC/NTP yet).

## Extra repo utilities (not in monacc list)

| Tool | Status | Flags/Features (#) |
|---|---:|---:|
| pid | Done | 1 |
| brk | Done | 1 |
| mmap | Done | 1 |
| cwd | Done | 3 |
| tty | Done | 2 |
| compat | Done | 5 |

## Networking playground tools (proposed)

These are **ranked** by “how soon they become fun/useful” given the current state:

1. **ifconfig/ip6 (very small)** — show link + counters, optionally configure a local IPv6 address
	 - Useful when you have:
		 - A working `netif` driver path (Phase 2 L2 RX/TX)
		 - Some way to read interface state from userspace (done: `/proc/net`)
	 - Becomes *configurable* when you add:
		 - A userspace→kernel control API to set per-interface IPv6 address / bring link up (could be a tiny `ioctl`-style syscall or a minimal `socket` API)
		 - Optional: a place to persist/show config (e.g. `/proc/net/ifaces`, `/proc/net/ipv6_addr`)

2. **ping6 (ICMPv6 echo)** — the canonical “is the network alive?” tool
	 - Useful when you have:
		 - IPv6 packet build/parse + checksum
		 - ICMPv6 Echo Request/Reply
		 - Neighbor Discovery (NDP): at least NS/NA to resolve next-hop MAC (or a hardcoded neighbor entry as a temporary hack)
		 - A userspace API to send/receive ICMPv6 (raw IPv6/ICMP socket, or a dedicated `sys_ping6`/`sys_net_raw` interface)
	 - Extra polish:
		 - RTT timing (clock is already there), sequence numbers, `-c`, `-i`, `-W`

3. **udp6cat (netcat for UDP over IPv6)** — simplest “real payload” tool
	 - Useful when you have:
		 - IPv6 + UDP + checksum
		 - Basic port demux in-kernel
		 - A userspace API for datagrams (`socket(AF_INET6, SOCK_DGRAM)` + `sendto/recvfrom`, or a tiny custom UDP syscall pair)
	 - Nice-to-have:
		 - `-l` listen mode, `-p` local port, `-w` timeout

4. **netcat6 (TCP over IPv6)** — interactive testing, shells, file copy
	 - Useful when you have:
		 - TCP (3-way handshake, retransmit timers, basic congestion control can be minimal at first)
		 - Blocking reads/writes + wakeups (already have blocked IO patterns for UART/pipe)
		 - A userspace socket API (`socket/connect/listen/accept/read/write/close`)
	 - Note:
		 - This is the biggest jump in complexity; UDP tools give you most early value.

5. **dns6 (tiny resolver)** — makes higher-level tools possible
	 - Useful when you have:
		 - UDP working (DNS is usually UDP first)
		 - A way to configure DNS server IPv6 (hardcode, `/etc/resolv.conf`, or `/proc/net/dns`)

6. **wget6 (already listed as Planned)** — end-to-end proof
	 - Useful when you have:
		 - DNS + TCP + a minimal HTTP client
		 - A userspace socket API

7. **tcpdump/pcap-lite** — debugging superpower
	 - Useful when you have:
		 - A capture API (raw frame tap from `netif_rx_frame()`), plus a way to stream it to userspace
		 - Optional: simple capture filters (even “ethertype ipv6” is a big help)

If you want the most “elementary” possible tool that still exercises the driver path before IPv6 exists, a tiny `ethsend`/`ethrecv` (raw Ethernet frames) can work — but it really wants a raw-packet userspace API, and most external hosts won’t respond to arbitrary frames unless you implement at least NDP/IPv6.

## QEMU TAP networking (USB_NET_BACKEND=tap)

If you run:

- `make run USB_NET=1 USB_NET_BACKEND=tap`

and see an error like:

- `could not configure /dev/net/tun (...): Operation not permitted`

then QEMU doesn’t have permission to create/configure a TAP device.

### Quick fix (recommended): create a persistent TAP owned by your user

1. Create the TAP interface once (needs root):

- `sudo tools/setup-tap.sh mona0 "$USER"`

2. Run QEMU using that interface:

- `make run USB_NET=1 USB_NET_BACKEND=tap TAP_IF=mona0`

### Alternatives

- Run QEMU as root (simplest, but not ideal): `sudo -E make run USB_NET=1 USB_NET_BACKEND=tap TAP_IF=mona0`
- Grant QEMU capabilities (system-wide change): `sudo setcap cap_net_admin+ep $(command -v qemu-system-aarch64)`

### Note on “getting packets to/from the outside”

Creating a TAP only gives you a host L2 interface. To reach the wider network you typically bridge it (or route/NAT) and/or run a router advertisement daemon on the host.

For QEMU *user-mode* networking (`USB_NET_BACKEND=user`), Router Advertisements typically do **not** include RDNSS. In that mode QEMU commonly provides an IPv6 DNS server at `fec0::3` (slirp DNS). The userland `dns6`/`ping6` defaults try RA RDNSS first, then `fec0::3`, then fall back to Google DNS.
