# video.md — Framebuffer & FullHD Terminal Plan (QEMU-first, Pi Zero 2 W later)

Date: 2025-12-30

Goal: add **framebuffer output** and a **high-resolution (1920×1080) terminal** to the kernel, validated in **QEMU (-M raspi3b)** first and then moved toward **real Raspberry Pi Zero 2 W (BCM2710A1)**.

Constraints (project style):
- No external runtime deps.
- Keep `make test` / UART workflow intact; framebuffer is additive.
- Prefer small, testable steps with clear “done” criteria.

---

## Phase 0 — Decide the initial display path (keep it simple)

Start with **Pi mailbox framebuffer** (aka “firmware framebuffer”) via the **property mailbox interface**:
- Works on real Pi firmware.
- QEMU’s Raspberry Pi machine models commonly emulate enough mailbox functionality to display a framebuffer.
- Lets us avoid full VC4 KMS/DRM complexity initially.

Later (optional, long-term) add a VC4/KMS path (more Linux-like, more complex).

**Acceptance for Phase 0**:
- We can request a framebuffer at boot and draw pixels.

---

## Phase 1 — Minimal framebuffer bring-up (solid API)

### 1.1 Add a small framebuffer descriptor + init API
Create a new module (suggested):
- `kernel-aarch64/include/fb.h`
- `kernel-aarch64/fb.c`

Data structure (suggested fields):
- `uint32_t width, height;`
- `uint32_t pitch;` (bytes per row)
- `uint32_t bpp;` (bits per pixel, start with 32)
- `uint64_t phys_addr;` (GPU/firmware returned bus addr)
- `void *virt;` (kernel VA mapping)
- `uint32_t format;` (optional; assume XRGB8888 first)

Provide:
- `int fb_init_from_mailbox(uint32_t req_w, uint32_t req_h, uint32_t req_bpp);`
- `void fb_put_pixel(uint32_t x, uint32_t y, uint32_t argb);`
- `void fb_fill(uint32_t argb);`

Keep this module “dumb”: no terminal logic here.

### 1.2 Implement Pi mailbox property calls (kernel-side)
You likely want a new mailbox module (or extend an existing one if present):
- `kernel-aarch64/include/mailbox.h`
- `kernel-aarch64/mailbox.c`

Implement the **property channel** flow:
- Build a properly aligned message buffer.
- Write to mailbox channel.
- Poll for response.
- Parse returned values.

Property tags needed (typical set):
- Set physical size: 1920×1080
- Set virtual size: 1920×1080
- Set depth: 32
- Set pixel order: RGB (if available)
- Allocate framebuffer + alignment
- Get pitch

Notes:
- Returned framebuffer address may be a bus address; you must translate/mask as needed.
- For correctness on real hardware, expect cache attributes to matter (see Phase 3.3).

### 1.3 Map framebuffer into the kernel VA space
In `mmu.c` (or wherever mappings are created), map the framebuffer region:
- Strongly consider **device / non-cacheable** mapping initially to avoid cache-coherency surprises.
- Later optimize (write-combine / cacheable + explicit flush) if needed.

**Acceptance for Phase 1**:
- On boot, QEMU shows a solid-color screen (e.g., fill with a gradient or color bars).
- UART remains functional.

---

## Phase 2 — High-resolution text console (terminal renderer)

### 2.1 Pick a font strategy (start with built-in bitmap)
For a FullHD terminal you want a small font:
- 8×16: 240×67 characters
- 8×8: 240×135 characters
- 9×16 / 12×24: fewer columns, larger text

Start with a compact built-in bitmap font stored in the kernel:
- ASCII 32..126 (printables) is enough initially.
- Add CP437/Unicode later if desired.

Files:
- `kernel-aarch64/include/font8x16.h` (or similar)
- `kernel-aarch64/font8x16.c`

### 2.2 Implement a terminal state machine on top of fb
New module (suggested):
- `kernel-aarch64/include/termfb.h`
- `kernel-aarch64/termfb.c`

Terminal features, in order:
1) Text buffer: `cols × rows` of `{char, fg, bg}`.
2) Cursor, newline, carriage return, backspace.
3) Scroll up (memmove rows) + clear last row.
4) Fast path rendering:
   - Render only the glyph cell that changed.
   - When scrolling, either redraw all or implement row blit.

Color format:
- Use 32bpp XRGB8888 in memory.

**Acceptance for Phase 2**:
- Kernel prints boot log to framebuffer.
- Scrolling works.
- Cursor works.

### 2.3 ANSI subset (only what you need)
Implement a small subset of ANSI escape sequences:
- `\x1b[2J` clear screen
- `\x1b[H` home
- `\x1b[row;colH` cursor move
- `\x1b[0m` reset
- `\x1b[30–37m` fg colors
- `\x1b[40–47m` bg colors

Keep it minimal; you can extend as actual userland programs demand it.

**Acceptance for Phase 2.3**:
- `sh` feels usable in the framebuffer terminal for basic interaction.

---

## Phase 3 — Performance + correctness at FullHD

### 3.1 Avoid full-screen redraws
At 1920×1080×4 ≈ 8 MiB per frame, redrawing everything is expensive.

Do:
- Per-glyph dirty updates (default path).
- Optimize scroll:
  - Option A (simple): redraw all cells after scroll.
  - Option B (better): `memmove` framebuffer rows up by `glyph_h * pitch`, then render only the new line.

### 3.2 Optional: double buffering
If tearing is visible in QEMU or hardware:
- Allocate two framebuffers (or one framebuffer + one shadow buffer).
- Render to back buffer then copy/flip.

Mailbox firmware sometimes supports virtual offset for page-flip style behavior; if available:
- Use “set virtual offset” tag to flip.

### 3.3 Cache maintenance strategy (hardware-critical)
On real Pi, framebuffer memory can be:
- Uncached device memory (easy, slower CPU writes), or
- Cacheable + explicit cache clean/invalidate (faster, more complex).

Start with uncached mapping. If performance is insufficient:
- Make it write-combine (if your MMU setup supports it), or
- Keep it cacheable and clean dcache lines for updated regions.

**Acceptance for Phase 3**:
- FullHD console is responsive (typing feels immediate).
- No “missing pixels” / stale glyphs on real hardware.

---

## Phase 4 — Integrate with the existing TTY/console plumbing

Right now your interactive path is UART/PL011 + a tty-ish layer.

### 4.1 Add a console multiplexer
Create a small abstraction:
- `console_write(const char*, size_t)` fans out to:
  - UART console
  - framebuffer terminal

Keep UART as the “always works” channel.

### 4.2 Keyboard input (QEMU vs hardware)
Full terminal UX needs input beyond UART.

Short-term options:
- Keep input over UART (no keyboard).
- In QEMU, consider a simple emulated HID path if available on raspi3b machine.

Long-term (hardware):
- USB host stack + HID keyboard is a large project; defer unless you want a full local console.

**Acceptance for Phase 4**:
- Same userland works; output appears on both UART and framebuffer.

---

## Phase 5 — QEMU specifics (make it visible)

Ensure your QEMU run path enables a display:
- If you have a script (e.g. `tools/run-qemu-raspi3b.sh`), ensure it uses something like:
  - `-display cocoa` (macOS) or `-display sdl/gtk` depending on your setup
  - (Optionally) `-serial stdio` stays enabled

Add a small doc snippet to the script/comments:
- “Framebuffer output appears in QEMU window; UART still on stdio.”

**Acceptance for Phase 5**:
- `make test` stays headless-friendly (UART-based tests).
- A separate `make run` (or script) shows the framebuffer.

---

## Phase 6 — Toward real Pi Zero 2 W

### 6.1 Confirm mailbox base + DTB discovery
For hardware you must locate the mailbox registers correctly and not rely on QEMU-only assumptions.
- Use DTB to locate peripheral base / mailbox region if your DTB provides it.
- Keep addresses centralized.

### 6.2 Handle framebuffer address translation correctly
On real Pi firmware, returned addresses may need masking (e.g. bus-to-phys conversion).
- Make this a small, well-documented helper.

### 6.3 Validate on hardware incrementally
Bring-up order on a real Pi:
1) UART boot log (already)
2) Mailbox property “ping” (get board model/revision)
3) Allocate framebuffer + fill color
4) Draw a few glyphs
5) Run the same userland shell

---

## Suggested concrete milestone sequence (fast feedback)

1) **FB init + solid fill** (QEMU)
2) **Draw a pixel + rectangle + color bars**
3) **Glyph rendering** (one char)
4) **Basic terminal** (newlines + scroll)
5) **ANSI subset**
6) **Performance (scroll blit, dirty cells)**
7) **Hardware validation** (mailbox + fb)

Each milestone should keep UART output working and should not break `make test`.
