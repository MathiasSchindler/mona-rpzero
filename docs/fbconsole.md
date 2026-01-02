# Framebuffer console improvement paths

This repo’s framebuffer console is implemented in `kernel-aarch64/termfb.c` on top of the mailbox framebuffer in `kernel-aarch64/fb.c`.

Today it is intentionally minimal:

- Bitmap font: 5×7 glyphs drawn into a 6×8 cell (`TERMFB_FONT_W/H`).
- Color model: 32bpp XRGB8888.
- Minimal ANSI CSI handling: cursor positioning and basic SGR color codes (8 colors + bright variants).
- Scrolling: uses virtual framebuffer y-offset when available for efficient scrolling; falls back to copy+clear.

The current default “brown-ish” look is mostly a palette/theme issue: the ANSI “yellow” entry is closer to orange/brown.

This doc lists incremental improvement paths, ordered from “quick wins” to more ambitious upgrades.

---

## 1) Quick theme/palette fix (highest ROI)

### Goals

- Make the default look pleasant (and readable) immediately.
- Keep the existing fast, dense font and rendering pipeline.

### Changes

1. **Choose a better default foreground/background** for the framebuffer console on boot.
   - Example (high contrast):
     - Background: near-black `0x00101010`
     - Foreground: light gray `0x00e0e0e0`

2. **Adjust the built-in ANSI palette** used by SGR 30–37 / 40–47 (and bright variants).
   - Example “more standard” 8-color base:

     ```c
     // XRGB8888
     black   = 0x00000000;
     red     = 0x00aa0000;
     green   = 0x0000aa00;
     yellow  = 0x00aaaa00;   // avoid brown/orange
     blue    = 0x000000aa;
     magenta = 0x00aa00aa;
     cyan    = 0x0000aaaa;
     white   = 0x00aaaaaa;   // dim white
     ```

   - Bright variants can be tuned for readability (and not too neon).

### Why this helps

- It directly fixes the “unusual brown background” / odd overall tint without changing any rendering logic.

---

## 2) Make colors configurable (iterate fast)

### Goals

- Make it easy to experiment with palettes and themes without repeatedly editing low-level rendering code.

### Changes

- Introduce a small “theme” structure:
  - Default fg/bg colors
  - 16-entry ANSI palette (8 base + 8 bright)

- Provide an API such as:
  - `termfb_set_theme(const termfb_theme_t *t)`
  - or `termfb_set_palette(const uint32_t colors[16])`

### Possible configuration sources

- Compile-time selection (e.g. `TERMFB_THEME=linux`, `TERMFB_THEME=gruvbox`)
- Kernel config option / build flag
- Later: runtime toggle via a debug command or a simple `/proc` knob

---

## 3) Font upgrades (where most of the “ugly” comes from)

The current 5×7 glyphs are extremely practical (dense), but they look like a tiny retro bitmap.

### Options (in increasing effort)

1. **Replace the glyph table with a nicer bitmap font** of similar size
   - Keep 6×8 or 8×8 monospace
   - Better glyph design improves perceived quality without changing layout much

2. **Add a second font mode**
   - Keep the current dense mode
   - Add a larger 8×16 mode for readability
   - Choose font mode at boot (or allow switching later)

3. **Add integer scaling**
   - Render 1× (dense) or 2× (large, still crisp)
   - Useful when the framebuffer resolution is high

### Implementation notes

- Avoid hand-editing huge C arrays:
  - Store a font bitmap in the repo
  - Generate a C header during the build (simple script)

---

## 4) Terminal UX polish (small changes that feel big)

### Cursor

- Draw an underline or block cursor
- Invert the current cell or draw a small rectangle

### More SGR features

Currently SGR is mostly colors. Useful additions:

- `7` / `27`: inverse video
- `1` / `22`: bold on/off (optional: map to bright fg)
- `39` / `49`: reset fg/bg only

### Clear/erase behaviors

Beyond clear-screen (CSI `J` mode 2), consider:

- Clear to end-of-line
- Clear to end-of-screen

These matter a lot for interactive tools and for a more “real terminal” feel.

---

## 5) Rendering performance + smoothness

If we increase font size or add cursor blink, we may want to reduce framebuffer writes.

### Options

- **Cell grid + dirty tracking**
  - Maintain a shadow character buffer and only redraw changed cells

- **Double buffering (logical)**
  - Not necessarily a second pixel buffer; just avoid repainting unchanged areas

- **Keep fast scrolling**
  - The current virtual-y-offset scrolling is a big win and should stay

---

## Suggested next steps

A sensible incremental roadmap:

1. **Fix default theme + ANSI palette** (quick win, minimal risk).
2. **Add theme/palette configurability** (fast iteration, stable API).
3. **Add a nicer 8×16 font mode** (big UX improvement).
4. Add cursor + a couple of common SGR behaviors.

If you want to keep the dense look but improve aesthetics, step (1) + a better 6×8 font is typically the sweet spot.
