# Plan: Split `kernel-aarch64/exceptions.c`

This repository currently concentrates exception/trap handling, syscall dispatch, process management, file-descriptor logic, pipes, and a mini VFS (initramfs + overlay) into one translation unit: `kernel-aarch64/exceptions.c`.

This document proposes an **incremental, build-safe** refactor that keeps behavior stable while making the codebase easier to extend (notably for upcoming FS work like `touch`/`rm`).

---

## Current status (as of 2025-12-28)

Done (implemented + rebuilt + boot-smoke-tested in QEMU):

- [x] Step 0 — Baseline snapshot & mapping (baseline confirmed via rebuild/boot)
- [x] Step 1 — Extract VFS (initramfs + overlay)
   - Landed as `kernel-aarch64/vfs.c` + `kernel-aarch64/include/vfs.h`.
   - `exceptions.c` now calls `vfs_init()` during early proc init.
- [x] Step 2 — Extract pipes
   - Landed as `kernel-aarch64/pipe.c` + `kernel-aarch64/include/pipe.h`.
   - `sys_read`/`sys_write` delegate to `pipe_read`/`pipe_write`; `sys_pipe2` uses the pipe module.
- [x] Step 3 — Extract FD / file descriptions
   - Landed as `kernel-aarch64/fd.c` + `kernel-aarch64/include/fd.h`.
   - `g_descs` moved into `fd.c`; `exceptions.c` uses fd-table APIs.
- [x] Step 4 — Extract process + scheduler
   - Landed as `kernel-aarch64/proc.c` + `kernel-aarch64/include/proc.h` and `kernel-aarch64/sched.c` + `kernel-aarch64/include/sched.h`.
   - Register helpers moved to `kernel-aarch64/include/regs.h` and shared by modules.

Known nuance discovered while validating:

- The helper script `tools/run-qemu-raspi3b.sh` expects explicit arguments (and may not be executable). Invoke via `bash` and pass `--kernel/--dtb/--append`.

Remaining work (not done yet):

- [x] Step 5 — Shrink `exceptions.c` to “trap + dispatch” (optionally split syscall handlers by domain)
    - `kernel-aarch64/exceptions.c` now only contains `exception_report()` + `exception_handle()` (syscall dispatch) and delegates syscall bodies to modules.
    - New modules added:
       - `kernel-aarch64/sys_util.c` + `kernel-aarch64/include/sys_util.h` (uaccess + path helpers)
       - `kernel-aarch64/sys_fs.c` (filesystem/FD oriented syscalls)
       - `kernel-aarch64/sys_proc.c` (exec/clone/wait/exit, brk/mmap/munmap)
       - `kernel-aarch64/sys_misc.c` (uname/clock_gettime/getrandom/signal stubs)
       - `kernel-aarch64/power.c` + `kernel-aarch64/include/power.h` (poweroff + reboot)
       - `kernel-aarch64/include/errno.h` (shared errno numbers)
    - Note: the legacy monolithic implementation is currently still present in `exceptions.c` behind `#if 0` as an audit aid; it can be deleted once we’re fully confident.

This plan below is updated to reflect that Steps 1–3 are already completed.

---

## Goals

- Reduce the size and cognitive load of `kernel-aarch64/exceptions.c`.
- Create clear subsystem boundaries (proc/sched, fd, pipe, vfs/fs, syscalls).
- Preserve existing behavior and Linux-ish syscall ABI (return `-(errno)` in `x0`).
- Keep changes incremental: after each step, the kernel must still build and boot.

## Constraints / guardrails

- Prefer mechanical moves over rewrites: **move code first**, refactor later.
- Avoid changing syscall behavior during the split.
- Keep the syscall dispatcher in one place at first (easy to audit).
- Avoid a “big bang” change: move one subsystem at a time and test.

---

## Target structure (recommended)

Keep trap entry + syscall dispatch centralized, move subsystems out:

- `kernel-aarch64/exceptions.c`
  - Exception reporting (`exception_report`)
  - Trap entry and syscall dispatch (`exception_handle`)
  - Syscall-number defines (or move to a dedicated header once stable)

- `kernel-aarch64/proc.c` + `kernel-aarch64/include/proc.h`
  - `proc_t`, process table
  - `proc_init_if_needed`, process lifecycle
  - `sys_clone`, `sys_wait4`, exit handling

- `kernel-aarch64/sched.c` + `kernel-aarch64/include/sched.h`
  - runnable selection / time-slicing helpers
  - `proc_switch_to`, `sched_maybe_switch` (or whatever current names are)

- `kernel-aarch64/fd.c` + `kernel-aarch64/include/fd.h`
  - `file_desc_t`, descriptor allocation/refcount
  - per-process fd-table helpers

- `kernel-aarch64/pipe.c` + `kernel-aarch64/include/pipe.h`
  - `pipe_t`
  - `sys_pipe2` and pipe read/write semantics

- `kernel-aarch64/vfs.c` + `kernel-aarch64/include/vfs.h`
  - initramfs+overlay path lookup + directory listing
  - `ramdir_t` overlay (and later `ramfile` overlay)

You can also optionally group syscall handlers by area:

- `kernel-aarch64/sys_fs.c` (openat, mkdirat, getdents64, newfstatat, etc.)
- `kernel-aarch64/sys_proc.c` (clone, wait4, execve, exit)
- `kernel-aarch64/sys_misc.c` (uname, clock_gettime, getrandom, etc.)

…but the simplest first pass is: **keep syscalls with their subsystem**.

---

## Step-by-step split plan (incremental)

### Step 0 — Baseline snapshot & quick mapping

1. Build+boot to confirm current baseline.
   - Command: `make run`
2. Create a quick map of what lives where in `kernel-aarch64/exceptions.c`:
   - Trap entry / syscall dispatch
   - User copy helpers
   - Proc/sched
   - FD + pipe
   - VFS + initramfs overlay
   - Syscall implementations

Acceptance criteria:
- Baseline builds and boots.

---

### Step 1 — Extract VFS (initramfs + overlay)

Rationale: VFS code is relatively self-contained and is the area you’ll extend next (ramfiles/unlink).

1. Create new files:
   - `kernel-aarch64/vfs.c`
   - `kernel-aarch64/include/vfs.h`
2. Move these from `exceptions.c` into `vfs.c`:
   - `ramdir_t` definition (or keep it private to `vfs.c`)
   - `g_ramdirs` storage (recommended: keep owned by `vfs.c`)
   - helpers: `strip_leading_slashes_const`, `ramdir_find`, `ramdir_alloc_slot`
   - VFS API: `vfs_lookup_abs`, `vfs_list_dir` and its helper callbacks/structures
3. Decide on an API surface in `vfs.h`, for example:
   - `int vfs_lookup_abs(...);`
   - `int vfs_list_dir(...);`
   - `void vfs_init(void);` (to clear overlay state during boot)
4. Update `proc_init_if_needed` (or equivalent init path) to call `vfs_init()` instead of directly clearing `g_ramdirs`.
5. Update includes:
   - `exceptions.c` includes `vfs.h`
6. Update the kernel build to compile `vfs.c`.
   - Add `vfs.o` to `kernel-aarch64/Makefile`.

Acceptance criteria:
- Kernel builds.
- Boot still works.
- `ls` still lists initramfs entries.
- `mkdirat` overlay behavior still works (directories show up via `getdents64`).

Status: Completed.

---

### Step 2 — Extract pipes

Rationale: Pipe code is self-contained and frequently touched when adding shell features.

1. Create new files:
   - `kernel-aarch64/pipe.c`
   - `kernel-aarch64/include/pipe.h`
2. Move:
   - `pipe_t` struct and `g_pipes` storage
   - pipe helpers used by read/write paths
   - `sys_pipe2` implementation
   - any pipe-related parts of `sys_read`, `sys_write`, `sys_close` as needed
3. Expose minimal APIs to FD/syscall layer (for example, `pipe_read(...)`, `pipe_write(...)`, `pipe_close_end(...)`).
4. Update build (`pipe.o`).

Acceptance criteria:
- Kernel builds and boots.
- Shell pipelines still work (e.g. `seq 1 3 | wc -l`).

Status: Completed.

---

### Step 3 — Extract FD / file descriptions

Rationale: FD bookkeeping is used everywhere and grows quickly; isolating it reduces cross-coupling.

1. Create new files:
   - `kernel-aarch64/fd.c`
   - `kernel-aarch64/include/fd.h`
2. Move:
   - `file_desc_t` + enum `fdesc_kind_t` (or split kinds into separate headers)
   - descriptor allocation/refcounting
   - fd-table operations (`fd_get_desc_idx`, `fd_alloc_into`, `fd_close`, etc.)
   - `g_descs` storage
3. Make syscall handlers call into FD APIs.
4. Update build (`fd.o`).

Acceptance criteria:
- Kernel builds and boots.
- `openat/read/write/close/dup3` continue to function.

Status: Completed.

---

### Step 4 — Extract process + scheduler

Rationale: This is the largest/most cross-cutting piece; do it once FD/VFS/pipe boundaries exist.

1. Create new files:
   - `kernel-aarch64/proc.c` + `kernel-aarch64/include/proc.h`
   - `kernel-aarch64/sched.c` + `kernel-aarch64/include/sched.h`
2. Move:
   - `proc_t` struct, `g_procs` storage, `g_cur_proc`, pid allocation
   - `proc_init_if_needed`, `proc_clear`, `proc_close_all_fds`
   - scheduler selection and switching helpers
3. Keep `exception_handle` in `exceptions.c`, but have it call into proc/sched APIs.
4. Update build (`proc.o`, `sched.o`).

Acceptance criteria:
- Kernel builds and boots.
- `execve` still starts userland.
- `clone/wait4/exit` still behave.

Status: Completed.

Notes on what’s still in `exceptions.c` today (candidates to move):

- Process table and process lifecycle: `proc_t`, `g_procs`, pid allocation, `proc_clear`, `proc_init_if_needed`, `proc_close_all_fds`.
- Scheduling and context switching: runnable selection, `proc_switch_to`, any time-slicing helpers.
- VMAs and mmap/brk bookkeeping (still tightly coupled to `proc_t`): the `vma_t` array, `sys_mmap`, `sys_munmap`, and helpers.
- Path/cwd resolution state on the process (`cwd`) and helpers like `resolve_path`.

---

### Step 5 — Shrink `exceptions.c` to “trap + dispatch”

After steps 1–4, `exceptions.c` should primarily contain:

- syscall number defines (or move to `include/syscall_numbers.h`)
- `exception_report`
- `exception_handle` and the `switch (nr)` dispatcher

Optionally, create thin per-domain syscall files:

- `kernel-aarch64/sys_fs.c` (mkdirat/openat/getdents64/newfstatat)
- `kernel-aarch64/sys_proc.c` (execve/clone/wait4/exit)
- `kernel-aarch64/sys_misc.c` (uname/clock_gettime/getrandom/reboot)

Acceptance criteria:
- `exceptions.c` is substantially smaller and easy to audit.

Status: Not started.

---

## Validation checklist after each step

Minimum checks (fast):
- `make -C kernel-aarch64` (or top-level `make`)
- `make run`

Behavioral smoke tests in the booted environment:
- `ls /`
- `mkdir /tmp && ls /`
- `seq 1 10 | wc -l`

---

## Notes / expected follow-ups

- Once `vfs.c` exists, adding `ramfile` + `unlinkat` becomes a focused change in that module plus a small syscall stub.
- If you want to reduce globals, the next refactor (after the split) is to introduce a `kernel_state` struct and pass it around. That’s a bigger change and not needed for the first split.
