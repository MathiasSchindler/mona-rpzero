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
| env | Planned | 0 |
| rev | Partial | 2 |
| true | Done | 1 |
| expr | Planned | 0 |
| rm | Done | 4 |
| rmdir | Done | 1 |
| uname | Partial | 1 |
| cat | Partial | 2 |
| false | Done | 1 |
| uncpio | Planned | 0 |
| chmod | Planned | 0 |
| find | Partial | 5 |
| mkdir | Partial | 3 |
| sed | Partial | 4 |
| uniq | Partial | 4 |
| chown | Planned | 0 |
| free | Planned | 0 |
| more | Planned | 0 |
| seq | Partial | 3 |
| uptime | Done | 2 |
| mount | Planned | 0 |
| sh | Partial | 4 |
| watch | Planned | 0 |
| clear | Planned | 0 |
| mv | Planned | 0 |
| sha256 | Planned | 0 |
| wc | Partial | 4 |
| cmp | Planned | 0 |
| grep | Partial | 7 |
| sleep | Done | 2 |
| wget6 | Planned | 0 |
| col | Planned | 0 |
| gunzip | Planned | 0 |
| nl | Planned | 0 |
| which | Planned | 0 |
| column | Planned | 0 |
| gzip | Planned | 0 |
| sort | Partial | 4 |
| who | Planned | 0 |
| cp | Planned | 0 |
| head | Partial | 4 |
| stat | Planned | 0 |
| whoami | Planned | 0 |
| cpio | Planned | 0 |
| hexdump | Planned | 0 |
| objdump | Planned | 0 |
| strings | Planned | 0 |
| cut | Planned | 0 |
| od | Planned | 0 |
| tail | Partial | 4 |
| date | Done | 1 |
| hostname | Planned | 0 |
| paste | Planned | 0 |
| tar | Planned | 0 |
| xargs | Planned | 0 |
| df | Planned | 0 |
| id | Planned | 0 |
| xxd | Planned | 0 |
| diff | Planned | 0 |
| init | Partial | 1 |
| printf | Partial | 4 |
| tee | Partial | 3 |
| yes | Planned | 0 |
| dirname | Planned | 0 |
| kill | Done | 3 |
| ps | Done | 2 |
| test | Planned | 0 |

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
