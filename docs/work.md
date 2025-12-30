# work.md — Priorities & Implementation Guide

Date: 2025-12-30

This document is an actionable work plan for this repo’s **bare-metal AArch64 kernel + syscall-only userland**.

## Non-negotiables

- **No external runtime dependencies** (no libc, no external C deps, no third-party libraries).
- **QEMU-first (raspi3b)** remains the daily driver for iteration.
- **Move steadily toward Raspberry Pi Zero 2 W hardware** by:
  - avoiding QEMU-only device assumptions,
  - validating DTB usage and MMIO addresses match the real board,
  - keeping debug paths (UART) robust.

## Current state (quick)

- `make test` boots QEMU and runs `kinit` selftests successfully.
- Core syscalls include process model (`fork/exec/wait`), pipes/dup, initramfs + RAM overlay FS, and time.

---

## Priority 1 (P1): Compatibility improvements (high ROI)

### P1.1 Tighten symlink semantics

**Why**: We have working symlinks, but semantics/errno are not Linux-like yet. Fixing this early prevents userland oddities.

**Target files**:
- `kernel-aarch64/sys_fs.c`

**Implementation guide**:
- Return `-ELOOP` when symlink resolution exceeds hop limit (instead of `-EINVAL`).
- Consider adding `AT_SYMLINK_NOFOLLOW` handling to `newfstatat`.
  - This unlocks a clean future `lstat` tool.
- Ensure `execve` follows symlinks too (if it doesn’t already).

**Acceptance**:
- Add `kinit` test:
  - create `ln -s /bin/sh /tmp/sh` and run `/tmp/sh -c "echo ok"`.

### P1.2 Initramfs symlinks (build-time)

**Why**: Today, symlinks are overlay-only; initramfs cannot contain them.

**Target files**:
- `tools/mkcpio_newc.py`
- `kernel-aarch64/cpio_newc.c` / initramfs handling (only if needed)

**Implementation guide**:
- Extend packer to detect symlinks (via `Path.is_symlink()` / `os.readlink`) and emit CPIO entries with `S_IFLNK` and data = link target.
- Ensure kernel initramfs lookup returns correct `mode` and `data/size` for symlinks.

**Acceptance**:
- Add one initramfs symlink (e.g. in `userland/initramfs/`):
  - `/bin/[something]` → `/bin/sh` (or similar).
- `kinit` verifies `readlink` prints expected target.

---

## Priority 2 (P2): Make the userland more usable (coreutils-ish)

This increases day-to-day interactivity and also forces better kernel behavior.

### P2.1 `ls` improvements (most impactful day-to-day)

**Why**: current `ls` is intentionally minimal; making it practical improves workflow a lot.

**Suggested minimal feature set**:
- `ls [PATH...]` (default `.`)
- `-a` include dotfiles
- `-l` long format (mode, nlink, size)

**Kernel support needed**:
- Possibly improve `getdents64` and `newfstatat` behaviors.

**Acceptance**:
- `kinit` smoke:
  - `ls /bin` includes `sh`
  - `ls -l /bin/sh` prints mode/size

### P2.2 Add `stat` (unlocks many scripts)

**Why**: `stat` is the backbone for many diagnostics and drives kernel correctness (modes, sizes, symlinks).

**Implementation guide**:
- Use `newfstatat(AT_FDCWD, path, &st, flags)`.
- Print key fields only: mode, size, nlink.
- Add `-L` to follow symlinks (default), and `-P`/`--no-dereference` if you implement `AT_SYMLINK_NOFOLLOW`.

**Acceptance**:
- `kinit`:
  - `ln -s /uniq.txt /tmp/sy; stat /tmp/sy` behaves as expected.

### P2.3 `mv` via `renameat(2)` (or `renameat2`)

**Why**: `mv` is core UX and forces a well-defined FS mutation semantics.

**Kernel work**:
- Implement `renameat` or `renameat2` for overlay entries.
- Start simple: same-directory renames, regular files only.

**Acceptance**:
- `kinit`: `touch /tmp/a; mv /tmp/a /tmp/b; test -e /tmp/b`.

---

## Priority 3 (P3): Hardware path (Pi Zero 2 W readiness)

This is about shrinking the QEMU→hardware gap with measurable steps.

### P3.1 Hardware bring-up checklist doc

**Why**: keeps “Pi-later” work concrete and prevents QEMU-only drift.

**Add**:
- New doc: `hardware.md` (or a section in `plan.md`).

**Include**:
- UART base/clock expectations for Zero 2 W.
- DTB source of truth and how to use firmware-provided DTB.
- Expected initial EL and what’s verified.

### P3.2 Replace/guard QEMU-only behavior

**Targets**:
- semihosting poweroff/exit paths

**Implementation guide**:
- Keep semihosting behind `#ifdef QEMU_SEMIHOSTING` (already used).
- Provide a safe fallback for real hardware (spin, WFE loop, or watchdog reset later).

**Acceptance**:
- QEMU still exits cleanly for `make test`.

---

## Notes / constraints reminders

- Keep kernel changes **incremental** and backed by `kinit` tests.
- Prefer simple, deterministic semantics over completeness.
- Avoid feature creep (e.g. networking) until FS + process + tooling are solid.

## Suggested next 1–2 PRs

1) **P1.1** (symlink errno + execve follow)
  - Improves compatibility and reduces surprises.
2) **P2.1** (`ls` usability improvements)
  - High ROI for interactive development.

---

## Completed

- 2025-12-30: Unify path/parent parsing via `abs_path_to_no_slash_trim()` and `abs_path_parent_dir()` in [kernel-aarch64/sys_util.c](kernel-aarch64/sys_util.c) and [kernel-aarch64/include/sys_util.h](kernel-aarch64/include/sys_util.h).
- 2025-12-30: Centralize file type/mode bits in [kernel-aarch64/include/stat_bits.h](kernel-aarch64/include/stat_bits.h) and update callers.
- 2025-12-30: Consolidate syscall numbers into [abi/syscall_numbers.h](abi/syscall_numbers.h) and include it from both kernel and userland to prevent drift.
