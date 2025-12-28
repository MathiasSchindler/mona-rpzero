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
| dmesg | Planned | 0 |
| kinit | Planned | 0 |
| pstree | Planned | 0 |
| time | Planned | 0 |
| awk | Planned | 0 |
| ln | Planned | 0 |
| pwd | Partial | 1 |
| basename | Planned | 0 |
| du | Planned | 0 |
| ls | Partial | 5 |
| readelf | Planned | 0 |
| touch | Planned | 0 |
| echo | Partial | 1 |
| readlink | Planned | 0 |
| tr | Planned | 0 |
| bpe | Planned | 0 |
| env | Planned | 0 |
| rev | Planned | 0 |
| true | Done | 1 |
| browse | Planned | 0 |
| expr | Planned | 0 |
| rm | Planned | 0 |
| uname | Partial | 1 |
| cat | Partial | 2 |
| false | Planned | 0 |
| rmdir | Planned | 0 |
| uncpio | Planned | 0 |
| chmod | Planned | 0 |
| find | Planned | 0 |
| mkdir | Partial | 3 |
| sed | Planned | 0 |
| uniq | Partial | 4 |
| chown | Planned | 0 |
| free | Planned | 0 |
| more | Planned | 0 |
| seq | Partial | 3 |
| uptime | Planned | 0 |
| mount | Planned | 0 |
| sh | Partial | 3 |
| watch | Planned | 0 |
| clear | Planned | 0 |
| mv | Planned | 0 |
| sha256 | Planned | 0 |
| wc | Partial | 4 |
| cmp | Planned | 0 |
| grep | Planned | 0 |
| sleep | Partial | 1 |
| wget6 | Planned | 0 |
| col | Planned | 0 |
| gunzip | Planned | 0 |
| nl | Planned | 0 |
| which | Planned | 0 |
| column | Planned | 0 |
| gzip | Planned | 0 |
| nproc | Planned | 0 |
| sort | Planned | 0 |
| who | Planned | 0 |
| cp | Planned | 0 |
| head | Planned | 0 |
| stat | Planned | 0 |
| whoami | Planned | 0 |
| cpio | Planned | 0 |
| hexdump | Planned | 0 |
| objdump | Planned | 0 |
| strings | Planned | 0 |
| wtf | Planned | 0 |
| cut | Planned | 0 |
| hkdf | Planned | 0 |
| od | Planned | 0 |
| tail | Planned | 0 |
| date | Planned | 0 |
| hostname | Planned | 0 |
| paste | Planned | 0 |
| tar | Planned | 0 |
| xargs | Planned | 0 |
| df | Planned | 0 |
| id | Planned | 0 |
| xxd | Planned | 0 |
| diff | Planned | 0 |
| init | Partial | 1 |
| printf | Planned | 0 |
| tee | Planned | 0 |
| yes | Planned | 0 |
| dirname | Planned | 0 |
| kill | Planned | 0 |
| ps | Planned | 0 |
| test | Planned | 0 |

Notes on current “Partial” implementations:

- `ls`: lists `/` only; no args/flags.
- `echo`: prints argv separated by spaces; no flags.
- `cat`: supports stdin→stdout and a single file argument.
- `uname`: prints `sysname release machine`; no flags.
- `sleep`: current binary is a syscall smoke test (fixed `nanosleep` request) rather than a CLI-compatible sleep.
- `sh`: supports interactive mode, `-c`, and a single `cmd1 | cmd2` pipeline.
- `init`: system init + selftests runner, not a general-purpose tool.

## Extra repo utilities (not in monacc list)

| Tool | Status | Flags/Features (#) |
|---|---:|---:|
| pid | Done | 1 |
| brk | Done | 1 |
| mmap | Done | 1 |
| cwd | Done | 3 |
| tty | Done | 2 |
| compat | Done | 5 |
