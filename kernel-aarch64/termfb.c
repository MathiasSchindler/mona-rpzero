#include "termfb.h"

#include "fb.h"

#define TERMFB_FONT_W 6u
#define TERMFB_FONT_H 8u

typedef struct glyph_def {
    char ch;
    uint8_t rows[7]; /* 5-bit rows (MSB is left-most pixel) */
} glyph_def_t;

static const glyph_def_t g_glyphs[] = {
    { ' ', { 0x00,0x00,0x00,0x00,0x00,0x00,0x00 } },

    /* Punctuation */
    { '!', { 0x04,0x04,0x04,0x04,0x04,0x00,0x04 } },
    { '"',{ 0x0A,0x0A,0x00,0x00,0x00,0x00,0x00 } },
    { '#', { 0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A } },
    { '%', { 0x19,0x19,0x02,0x04,0x08,0x13,0x13 } },
    { '&', { 0x0C,0x12,0x14,0x08,0x15,0x12,0x0D } },
    { '\'',{ 0x04,0x04,0x02,0x00,0x00,0x00,0x00 } },
    { '(', { 0x02,0x04,0x08,0x08,0x08,0x04,0x02 } },
    { ')', { 0x08,0x04,0x02,0x02,0x02,0x04,0x08 } },
    { '*', { 0x00,0x0A,0x04,0x1F,0x04,0x0A,0x00 } },
    { '+', { 0x00,0x04,0x04,0x1F,0x04,0x04,0x00 } },
    { ',', { 0x00,0x00,0x00,0x00,0x06,0x06,0x04 } },
    { '-', { 0x00,0x00,0x00,0x1F,0x00,0x00,0x00 } },
    { '.', { 0x00,0x00,0x00,0x00,0x00,0x06,0x06 } },
    { '/', { 0x01,0x02,0x04,0x08,0x10,0x00,0x00 } },
    { ':', { 0x00,0x06,0x06,0x00,0x06,0x06,0x00 } },
    { ';', { 0x00,0x06,0x06,0x00,0x06,0x06,0x04 } },
    { '<', { 0x02,0x04,0x08,0x10,0x08,0x04,0x02 } },
    { '=', { 0x00,0x00,0x1F,0x00,0x1F,0x00,0x00 } },
    { '>', { 0x08,0x04,0x02,0x01,0x02,0x04,0x08 } },
    { '?', { 0x0E,0x11,0x01,0x02,0x04,0x00,0x04 } },
    { '[', { 0x1E,0x10,0x10,0x10,0x10,0x10,0x1E } },
    { '\\',{ 0x10,0x08,0x04,0x02,0x01,0x00,0x00 } },
    { ']', { 0x1E,0x02,0x02,0x02,0x02,0x02,0x1E } },
    { '_', { 0x00,0x00,0x00,0x00,0x00,0x00,0x1F } },
    { '|', { 0x04,0x04,0x04,0x04,0x04,0x04,0x04 } },

    /* Digits */
    { '0', { 0x0E,0x11,0x13,0x15,0x19,0x11,0x0E } },
    { '1', { 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E } },
    { '2', { 0x0E,0x11,0x01,0x02,0x04,0x08,0x1F } },
    { '3', { 0x1F,0x02,0x04,0x02,0x01,0x11,0x0E } },
    { '4', { 0x02,0x06,0x0A,0x12,0x1F,0x02,0x02 } },
    { '5', { 0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E } },
    { '6', { 0x06,0x08,0x10,0x1E,0x11,0x11,0x0E } },
    { '7', { 0x1F,0x01,0x02,0x04,0x08,0x08,0x08 } },
    { '8', { 0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E } },
    { '9', { 0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C } },

    /* Uppercase */
    { 'A', { 0x0E,0x11,0x11,0x1F,0x11,0x11,0x11 } },
    { 'B', { 0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E } },
    { 'C', { 0x0E,0x11,0x10,0x10,0x10,0x11,0x0E } },
    { 'D', { 0x1C,0x12,0x11,0x11,0x11,0x12,0x1C } },
    { 'E', { 0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F } },
    { 'F', { 0x1F,0x10,0x10,0x1E,0x10,0x10,0x10 } },
    { 'G', { 0x0E,0x11,0x10,0x17,0x11,0x11,0x0F } },
    { 'H', { 0x11,0x11,0x11,0x1F,0x11,0x11,0x11 } },
    { 'I', { 0x0E,0x04,0x04,0x04,0x04,0x04,0x0E } },
    { 'J', { 0x07,0x02,0x02,0x02,0x02,0x12,0x0C } },
    { 'K', { 0x11,0x12,0x14,0x18,0x14,0x12,0x11 } },
    { 'L', { 0x10,0x10,0x10,0x10,0x10,0x10,0x1F } },
    { 'M', { 0x11,0x1B,0x15,0x15,0x11,0x11,0x11 } },
    { 'N', { 0x11,0x19,0x15,0x13,0x11,0x11,0x11 } },
    { 'O', { 0x0E,0x11,0x11,0x11,0x11,0x11,0x0E } },
    { 'P', { 0x1E,0x11,0x11,0x1E,0x10,0x10,0x10 } },
    { 'Q', { 0x0E,0x11,0x11,0x11,0x15,0x12,0x0D } },
    { 'R', { 0x1E,0x11,0x11,0x1E,0x14,0x12,0x11 } },
    { 'S', { 0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E } },
    { 'T', { 0x1F,0x04,0x04,0x04,0x04,0x04,0x04 } },
    { 'U', { 0x11,0x11,0x11,0x11,0x11,0x11,0x0E } },
    { 'V', { 0x11,0x11,0x11,0x11,0x11,0x0A,0x04 } },
    { 'W', { 0x11,0x11,0x11,0x15,0x15,0x15,0x0A } },
    { 'X', { 0x11,0x11,0x0A,0x04,0x0A,0x11,0x11 } },
    { 'Y', { 0x11,0x11,0x0A,0x04,0x04,0x04,0x04 } },
    { 'Z', { 0x1F,0x01,0x02,0x04,0x08,0x10,0x1F } },

    /* Lowercase */
    { 'a', { 0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F } },
    { 'b', { 0x10,0x10,0x1E,0x11,0x11,0x11,0x1E } },
    { 'c', { 0x00,0x00,0x0E,0x11,0x10,0x11,0x0E } },
    { 'd', { 0x01,0x01,0x0F,0x11,0x11,0x11,0x0F } },
    { 'e', { 0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E } },
    { 'f', { 0x06,0x08,0x1E,0x08,0x08,0x08,0x08 } },
    { 'g', { 0x00,0x0F,0x11,0x11,0x0F,0x01,0x0E } },
    { 'h', { 0x10,0x10,0x1E,0x11,0x11,0x11,0x11 } },
    { 'i', { 0x04,0x00,0x0C,0x04,0x04,0x04,0x0E } },
    { 'j', { 0x02,0x00,0x06,0x02,0x02,0x12,0x0C } },
    { 'k', { 0x10,0x10,0x11,0x12,0x1C,0x12,0x11 } },
    { 'l', { 0x0C,0x04,0x04,0x04,0x04,0x04,0x0E } },
    { 'm', { 0x00,0x00,0x1A,0x15,0x15,0x11,0x11 } },
    { 'n', { 0x00,0x00,0x1E,0x11,0x11,0x11,0x11 } },
    { 'o', { 0x00,0x00,0x0E,0x11,0x11,0x11,0x0E } },
    { 'p', { 0x00,0x00,0x1E,0x11,0x11,0x1E,0x10 } },
    { 'q', { 0x00,0x00,0x0F,0x11,0x11,0x0F,0x01 } },
    { 'r', { 0x00,0x00,0x16,0x19,0x10,0x10,0x10 } },
    { 's', { 0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E } },
    { 't', { 0x08,0x08,0x1E,0x08,0x08,0x09,0x06 } },
    { 'u', { 0x00,0x00,0x11,0x11,0x11,0x11,0x0F } },
    { 'v', { 0x00,0x00,0x11,0x11,0x11,0x0A,0x04 } },
    { 'w', { 0x00,0x00,0x11,0x11,0x15,0x15,0x0A } },
    { 'x', { 0x00,0x00,0x11,0x0A,0x04,0x0A,0x11 } },
    { 'y', { 0x00,0x00,0x11,0x11,0x0F,0x01,0x0E } },
    { 'z', { 0x00,0x00,0x1F,0x02,0x04,0x08,0x1F } },
};

static const uint8_t g_glyph_unknown[7] = { 0x1F,0x11,0x15,0x11,0x15,0x11,0x1F };

static const fb_info_t *g_info;
static uint32_t g_cols;
static uint32_t g_rows;
static uint32_t g_cur_x;
static uint32_t g_cur_y;
static uint32_t g_fg;
static uint32_t g_bg;
static uint32_t g_default_fg;
static uint32_t g_default_bg;

typedef enum termfb_ansi_state {
    TERMFB_ANSI_NORMAL = 0,
    TERMFB_ANSI_ESC,
    TERMFB_ANSI_CSI,
} termfb_ansi_state_t;

static termfb_ansi_state_t g_ansi_state = TERMFB_ANSI_NORMAL;
static uint32_t g_ansi_params[4];
static uint32_t g_ansi_param_count = 0;
static uint32_t g_ansi_current = 0;
static int g_ansi_have_current = 0;

static uint32_t termfb_ansi_color_to_xrgb8888(uint32_t color_idx, int bright) {
    /* Basic ANSI 8-color palette. */
    static const uint32_t base[8] = {
        0x00000000u, /* black */
        0x00aa0000u, /* red */
        0x0000aa00u, /* green */
        0x00aa5500u, /* yellow */
        0x000000aau, /* blue */
        0x00aa00aau, /* magenta */
        0x0000aaaau, /* cyan */
        0x00aaaaaau, /* white (dim) */
    };
    static const uint32_t brightv[8] = {
        0x00555555u,
        0x00ff5555u,
        0x0055ff55u,
        0x00ffff55u,
        0x005555ffu,
        0x00ff55ffu,
        0x0055ffffu,
        0x00ffffffu,
    };

    color_idx &= 7u;
    return bright ? brightv[color_idx] : base[color_idx];
}

static void termfb_set_cursor_1based(uint32_t row1, uint32_t col1) {
    if (row1 == 0) row1 = 1;
    if (col1 == 0) col1 = 1;
    uint32_t y = row1 - 1u;
    uint32_t x = col1 - 1u;
    if (y >= g_rows) y = g_rows - 1u;
    if (x >= g_cols) x = g_cols - 1u;
    g_cur_y = y;
    g_cur_x = x;
}

static const uint8_t *termfb_glyph_for(char c) {
    uint32_t n = (uint32_t)(sizeof(g_glyphs) / sizeof(g_glyphs[0]));
    for (uint32_t i = 0; i < n; i++) {
        if (g_glyphs[i].ch == c) {
            return g_glyphs[i].rows;
        }
    }
    return g_glyph_unknown;
}

static inline uint32_t termfb_map_view_y_to_phys(uint32_t view_y) {
    if (!g_info) return view_y;
    if (g_info->virt_height == 0) return view_y;
    return (uint32_t)(((uint64_t)view_y + (uint64_t)g_info->y_offset) % (uint64_t)g_info->virt_height);
}

static inline volatile uint32_t *termfb_row_ptr_phys(uint32_t phys_y) {
    return (volatile uint32_t *)((uintptr_t)g_info->virt + (uintptr_t)phys_y * (uintptr_t)g_info->pitch);
}

static inline volatile uint32_t *termfb_row_ptr_view(uint32_t view_y) {
    return termfb_row_ptr_phys(termfb_map_view_y_to_phys(view_y));
}

static void termfb_clear_pixels(void) {
    if (!g_info || !g_info->virt) return;
    if (g_info->bpp != 32) return;

    uint32_t total_h = g_info->virt_height ? g_info->virt_height : g_info->height;
    for (uint32_t y = 0; y < total_h; y++) {
        volatile uint32_t *row = termfb_row_ptr_phys(y);
        for (uint32_t x = 0; x < g_info->width; x++) {
            row[x] = g_bg;
        }
    }
}

static void termfb_clear_phys_rows(uint32_t phys_y_start, uint32_t rows) {
    if (!g_info || !g_info->virt) return;
    if (g_info->bpp != 32) return;
    if (rows == 0) return;

    uint32_t total_h = g_info->virt_height ? g_info->virt_height : g_info->height;
    for (uint32_t ry = 0; ry < rows; ry++) {
        uint32_t y = (phys_y_start + ry);
        if (total_h != 0) y %= total_h;
        volatile uint32_t *row = termfb_row_ptr_phys(y);
        for (uint32_t x = 0; x < g_info->width; x++) {
            row[x] = g_bg;
        }
    }
}

static void termfb_scroll_one(void) {
    if (!g_info || !g_info->virt) return;
    if (g_info->bpp != 32) return;

    /* Prefer hardware scrolling via virtual offset to avoid large memcpy on device memory. */
    if (g_info->virt_height > g_info->height && (g_info->height % TERMFB_FONT_H) == 0) {
        uint32_t total_h = g_info->virt_height;
        uint32_t new_off = (g_info->y_offset + TERMFB_FONT_H) % total_h;

        if (fb_set_virtual_offset(0, new_off) == 0) {
            /* Clear the newly exposed bottom character row. */
            uint32_t bottom_phys = (new_off + g_info->height - TERMFB_FONT_H) % total_h;
            termfb_clear_phys_rows(bottom_phys, TERMFB_FONT_H);
            return;
        }
    }

    uint32_t rows_to_move = g_info->height - TERMFB_FONT_H;
    uint32_t words_per_row = g_info->pitch / 4u;

    for (uint32_t y = 0; y < rows_to_move; y++) {
        volatile uint32_t *dst = termfb_row_ptr_phys(y);
        volatile uint32_t *src = termfb_row_ptr_phys(y + TERMFB_FONT_H);
        for (uint32_t x = 0; x < words_per_row; x++) {
            dst[x] = src[x];
        }
    }

    /* Clear last character row (TERMFB_FONT_H pixel rows). */
    for (uint32_t y = rows_to_move; y < g_info->height; y++) {
        volatile uint32_t *row = termfb_row_ptr_phys(y);
        for (uint32_t x = 0; x < g_info->width; x++) {
            row[x] = g_bg;
        }
    }
}

static void termfb_draw_char_cell(uint32_t cell_x, uint32_t cell_y, char c) {
    if (!g_info || !g_info->virt) return;
    if (g_info->bpp != 32) return;

    uint32_t px0 = cell_x * TERMFB_FONT_W;
    uint32_t py0 = cell_y * TERMFB_FONT_H;

    if (px0 + TERMFB_FONT_W > g_info->width) return;
    if (py0 + TERMFB_FONT_H > g_info->height) return;

    const uint8_t *rows = termfb_glyph_for(c);

    for (uint32_t ry = 0; ry < 7u; ry++) {
        uint8_t bits = rows[ry] & 0x1Fu;
        volatile uint32_t *row = termfb_row_ptr_view(py0 + ry);

        for (uint32_t rx = 0; rx < 5u; rx++) {
            uint32_t mask = 1u << (4u - rx);
            row[px0 + rx] = (bits & mask) ? g_fg : g_bg;
        }

        /* 1px spacing column */
        row[px0 + 5u] = g_bg;
    }

    /* Bottom spacing row */
    volatile uint32_t *bottom = termfb_row_ptr_view(py0 + 7u);
    for (uint32_t rx = 0; rx < TERMFB_FONT_W; rx++) {
        bottom[px0 + rx] = g_bg;
    }
}

int termfb_init(uint32_t fg_xrgb8888, uint32_t bg_xrgb8888) {
    g_info = fb_get_info();
    if (!g_info || !g_info->virt) return -1;
    if (g_info->bpp != 32) return -1;

    g_cols = g_info->width / TERMFB_FONT_W;
    g_rows = g_info->height / TERMFB_FONT_H;
    if (g_cols == 0 || g_rows == 0) return -1;

    g_fg = fg_xrgb8888;
    g_bg = bg_xrgb8888;
    g_default_fg = fg_xrgb8888;
    g_default_bg = bg_xrgb8888;

    g_cur_x = 0;
    g_cur_y = 0;

    g_ansi_state = TERMFB_ANSI_NORMAL;
    g_ansi_param_count = 0;
    g_ansi_current = 0;
    g_ansi_have_current = 0;

    termfb_clear_pixels();
    return 0;
}

void termfb_set_colors(uint32_t fg_xrgb8888, uint32_t bg_xrgb8888) {
    g_fg = fg_xrgb8888;
    g_bg = bg_xrgb8888;
}

void termfb_clear(void) {
    g_cur_x = 0;
    g_cur_y = 0;
    termfb_clear_pixels();
}

static void termfb_ansi_push_param(uint32_t v) {
    if (g_ansi_param_count < (uint32_t)(sizeof(g_ansi_params) / sizeof(g_ansi_params[0]))) {
        g_ansi_params[g_ansi_param_count++] = v;
    }
}

static void termfb_ansi_finish_param_if_needed(void) {
    if (g_ansi_have_current || g_ansi_param_count == 0) {
        termfb_ansi_push_param(g_ansi_have_current ? g_ansi_current : 0u);
    }
    g_ansi_current = 0;
    g_ansi_have_current = 0;
}

static void termfb_ansi_handle_csi(char final_byte) {
    /* Ensure at least one parameter exists (defaults to 0). */
    termfb_ansi_finish_param_if_needed();

    if (final_byte == 'm') {
        /* SGR: colors only (0 reset, 30-37 fg, 40-47 bg, 90-97 fg bright, 100-107 bg bright). */
        for (uint32_t i = 0; i < g_ansi_param_count; i++) {
            uint32_t p = g_ansi_params[i];
            if (p == 0) {
                g_fg = g_default_fg;
                g_bg = g_default_bg;
            } else if (p >= 30 && p <= 37) {
                g_fg = termfb_ansi_color_to_xrgb8888(p - 30u, 0);
            } else if (p >= 40 && p <= 47) {
                g_bg = termfb_ansi_color_to_xrgb8888(p - 40u, 0);
            } else if (p >= 90 && p <= 97) {
                g_fg = termfb_ansi_color_to_xrgb8888(p - 90u, 1);
            } else if (p >= 100 && p <= 107) {
                g_bg = termfb_ansi_color_to_xrgb8888(p - 100u, 1);
            }
        }
    } else if (final_byte == 'J') {
        /* Erase in display: support only 2 (clear entire screen). */
        uint32_t mode = g_ansi_params[0];
        if (mode == 2u) {
            termfb_clear();
        }
    } else if (final_byte == 'H' || final_byte == 'f') {
        /* CUP: cursor position (row;col), defaults to 1;1. */
        uint32_t row1 = (g_ansi_param_count >= 1) ? g_ansi_params[0] : 1u;
        uint32_t col1 = (g_ansi_param_count >= 2) ? g_ansi_params[1] : 1u;
        termfb_set_cursor_1based(row1, col1);
    }

    /* Reset CSI state. */
    g_ansi_param_count = 0;
    g_ansi_current = 0;
    g_ansi_have_current = 0;
    g_ansi_state = TERMFB_ANSI_NORMAL;
}

void termfb_putc(char c) {
    if (!g_info || !g_info->virt) return;

    if (c == '\r') {
        g_cur_x = 0;
        return;
    }

    if (c == '\n') {
        g_cur_x = 0;
        g_cur_y++;
        if (g_cur_y >= g_rows) {
            termfb_scroll_one();
            g_cur_y = g_rows - 1u;
        }
        return;
    }

    if (c == '\t') {
        uint32_t next = (g_cur_x + 4u) & ~3u;
        while (g_cur_x < next) {
            termfb_putc(' ');
        }
        return;
    }

    if (c == '\b') {
        if (g_cur_x > 0) {
            g_cur_x--;
        } else if (g_cur_y > 0) {
            g_cur_y--;
            g_cur_x = g_cols ? (g_cols - 1u) : 0;
        }
        termfb_draw_char_cell(g_cur_x, g_cur_y, ' ');
        return;
    }

    termfb_draw_char_cell(g_cur_x, g_cur_y, c);
    g_cur_x++;
    if (g_cur_x >= g_cols) {
        g_cur_x = 0;
        g_cur_y++;
        if (g_cur_y >= g_rows) {
            termfb_scroll_one();
            g_cur_y = g_rows - 1u;
        }
    }
}

void termfb_putc_ansi(char c) {
    if (!g_info || !g_info->virt) return;

    if (g_ansi_state == TERMFB_ANSI_NORMAL) {
        if ((unsigned char)c == 0x1B) {
            g_ansi_state = TERMFB_ANSI_ESC;
            return;
        }
        termfb_putc(c);
        return;
    }

    if (g_ansi_state == TERMFB_ANSI_ESC) {
        if (c == '[') {
            g_ansi_state = TERMFB_ANSI_CSI;
            g_ansi_param_count = 0;
            g_ansi_current = 0;
            g_ansi_have_current = 0;
            return;
        }

        /* Unknown ESC sequence: ignore and return to normal. */
        g_ansi_state = TERMFB_ANSI_NORMAL;
        return;
    }

    /* CSI */
    if (c >= '0' && c <= '9') {
        g_ansi_current = g_ansi_current * 10u + (uint32_t)(c - '0');
        g_ansi_have_current = 1;
        return;
    }

    if (c == ';') {
        termfb_ansi_push_param(g_ansi_have_current ? g_ansi_current : 0u);
        g_ansi_current = 0;
        g_ansi_have_current = 0;
        return;
    }

    /* Final byte */
    termfb_ansi_handle_csi(c);
}

void termfb_write_ansi(const char *s) {
    if (!s) return;
    while (*s) {
        termfb_putc_ansi(*s++);
    }
}

void termfb_write(const char *s) {
    if (!s) return;
    while (*s) {
        termfb_putc(*s++);
    }
}

void termfb_write_hex_u64(uint64_t v) {
    static const char hex[] = "0123456789abcdef";

    termfb_write("0x");

    for (int i = 15; i >= 0; i--) {
        uint8_t nib = (uint8_t)((v >> ((uint64_t)i * 4ull)) & 0xFull);
        termfb_putc(hex[nib]);
    }
}
