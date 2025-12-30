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

## Priority 0 (P0): Correctness + cleanup that prevents future bugs

These items reduce duplication and semantic drift. They are low-risk and increase confidence.

### P0.1 Unify path/parent parsing logic in the kernel

**Why**: `mkdirat/linkat/symlinkat` each re-implement “strip leading slashes, trim trailing slashes, find parent, verify parent is dir”. Duplication is a common source of subtle bugs and inconsistent errno.

**Target files**:
- `kernel-aarch64/sys_fs.c`
- (possibly) `kernel-aarch64/sys_util.c` (good home for shared helpers)

**Implementation guide**:
- Introduce helpers that operate on:
  - absolute path `"/a/b/c"`,
  - and/or normalized no-leading-slash path `"a/b/c"`.
- Suggested helpers:
  - `int path_parent_no_slash(const char *path_no_slash, char *parent_out, uint64_t cap)`
  - `int path_basename_no_slash(const char *path_no_slash, char *name_out, uint64_t cap)`
  - `int ensure_parent_dir_exists(const char *abs_path)`

**Acceptance**:
- `make test` unchanged.
- Add 1–2 small `kinit` checks around edge cases:
  - `mkdir /tmp/x/` works (or fails consistently),
  - `ln -s /uniq.txt /tmp/sy` still works.

### P0.2 Centralize file type/mode constants

**Why**: Mode bits appear in multiple places and some are ad-hoc.

**Target files**:
- New: `kernel-aarch64/include/stat_bits.h` (or similar)
- Update: `kernel-aarch64/sys_fs.c`, `kernel-aarch64/vfs.c`, and any other file that hardcodes `0170000u`, `0100000u`, `0040000u`.

**Implementation guide**:
- Define `S_IFMT`, `S_IFREG`, `S_IFDIR`, `S_IFLNK`, etc.
- Keep it tiny and internal (not “POSIX complete”).

**Acceptance**:
- `make test`.

### P0.3 Reduce syscall-number drift between kernel and userland

**Why**: syscall numbers are duplicated in `kernel-aarch64/exceptions.c` and `userland/include/syscall.h`.

**Implementation options (pick one)**:
1) **Single shared header** (preferred)
   - Create `abi/syscall_numbers.h` at repo root.
   - Include it from both kernel and userland.
2) **Generator** (still “no dependencies”): a tiny Python script to generate headers for kernel/userland.

**Acceptance**:
- `make test`.

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

1) **P0.1 + P0.2** (path helpers + mode constants)
   - Small diff, big long-term payoff.
2) **P1.1** (symlink errno + execve follow)
   - Improves compatibility and reduces surprises.
