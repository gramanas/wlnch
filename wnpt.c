/*
 * wnpt - tiny Wayland note prompt, companion to wlnch.
 *
 * Opens an OVERLAY layer-shell window, grabs the keyboard exclusively,
 * and lets the user type arbitrary UTF-8 text. Special keys:
 *
 *   Enter           commit: print the buffer to stdout and exit 0.
 *   Shift+Enter     insert a newline into the buffer.
 *   Backspace       delete the last codepoint.
 *   Ctrl+Backspace  delete the previous word.
 *   Ctrl+U          clear the entire buffer.
 *   Esc / Ctrl+G    abort: exit 1, nothing is written to stdout.
 *
 * Typical usage:
 *
 *   wnpt > note.txt
 *
 * Visual styling (font, colors, padding, corner radius, cursor width)
 * is shared with wlnch via config.h. The Wayland / FreeType / SHM
 * plumbing here is mostly verbatim from wlnch.c, by design — the two
 * tools are meant to look and feel like siblings rather than share a
 * library. Sections (numbered to mirror wlnch.c):
 *
 *   1. Includes
 *   2. Globals (text buffer, wayland, xkb, font, window state)
 *   3. Utility helpers (UTF-8, OOM)
 *   4. Text buffer
 *   5. Font / text rendering
 *   6. SHM buffer with release tracking
 *   7. Drawing
 *   8. Wayland listeners
 *   9. main()
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <fontconfig/fontconfig.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include "config.h"

/* ---------- 2. Globals ---------- */

/* The typed text. Always nul-terminated when len > 0; cap > len + 1.
 * Newlines in the buffer are real '\n' bytes inserted by Shift+Enter. */
static char   *g_text;
static size_t  g_text_len;
static size_t  g_text_cap;

static struct wl_display            * g_display;
static struct wl_registry           * g_registry;
static struct wl_compositor         * g_compositor;
static struct wl_shm                * g_shm;
static struct wl_seat               * g_seat;
static struct wl_keyboard           * g_keyboard;
static struct zwlr_layer_shell_v1   * g_layer_shell;
static struct wl_surface            * g_surface;
static struct zwlr_layer_surface_v1 * g_layer_surface;

static struct xkb_context           * g_xkb_ctx;
static struct xkb_keymap            * g_xkb_keymap;
static struct xkb_state             * g_xkb_state;

static FT_Library                     g_ft_lib;
static FT_Face                        g_ft_face;
static int                            g_line_height;
static int                            g_ascent;

static int                            g_running   = 1;
static int                            g_committed = 0; /* 1 => print on exit */
static int                            g_win_w     = 0;
static int                            g_win_h     = 0;

static const char *g_font_pattern = NULL;

/* ---------- 3. Utility helpers ---------- */

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "wnpt: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

/* Decode a single UTF-8 sequence at `s` into a codepoint, return number of
 * bytes consumed, or 0 on error. Bounded variant for non-nul-terminated
 * scans: stops at `s + max` even mid-sequence. */
static int utf8_decode_n(const char *s, size_t max, uint32_t *out) {
    if (max == 0) return 0;
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] < 0x80) { *out = u[0]; return 1; }
    if (max >= 2 && (u[0] & 0xE0) == 0xC0 && (u[1] & 0xC0) == 0x80) {
        *out = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F);
        return 2;
    }
    if (max >= 3 && (u[0] & 0xF0) == 0xE0 &&
        (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80) {
        *out = ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F);
        return 3;
    }
    if (max >= 4 && (u[0] & 0xF8) == 0xF0 && (u[1] & 0xC0) == 0x80 &&
        (u[2] & 0xC0) == 0x80 && (u[3] & 0xC0) == 0x80) {
        *out = ((u[0] & 0x07) << 18) | ((u[1] & 0x3F) << 12) |
               ((u[2] & 0x3F) << 6)  |  (u[3] & 0x3F);
        return 4;
    }
    return 0;
}

/* ---------- 4. Text buffer ---------- */

static void buf_append(const char *s, size_t n) {
    if (g_text_len + n + 1 > g_text_cap) {
        size_t newcap = g_text_cap ? g_text_cap * 2 : 64;
        while (newcap < g_text_len + n + 1) newcap *= 2;
        char *p = realloc(g_text, newcap);
        if (!p) die("out of memory");
        g_text     = p;
        g_text_cap = newcap;
    }
    memcpy(g_text + g_text_len, s, n);
    g_text_len += n;
    g_text[g_text_len] = '\0';
}

/* Drop the last UTF-8 codepoint from the buffer (a single Backspace). */
static void buf_pop_codepoint(void) {
    if (g_text_len == 0) return;
    size_t i = g_text_len;
    /* Walk back over continuation bytes (10xxxxxx), then one more byte
     * for the start of the sequence. */
    while (i > 0 && (((unsigned char)g_text[i - 1] & 0xC0) == 0x80))
        --i;
    if (i > 0) --i;
    g_text_len = i;
    g_text[i]  = '\0';
}

/* Drop trailing whitespace then the trailing word (Ctrl+Backspace). */
static void buf_pop_word(void) {
    while (g_text_len > 0 &&
           (g_text[g_text_len - 1] == ' ' || g_text[g_text_len - 1] == '\t'))
        buf_pop_codepoint();
    while (g_text_len > 0 &&
           g_text[g_text_len - 1] != ' '  &&
           g_text[g_text_len - 1] != '\t' &&
           g_text[g_text_len - 1] != '\n')
        buf_pop_codepoint();
}

static void buf_clear(void) {
    g_text_len = 0;
    if (g_text) g_text[0] = '\0';
}

/* ---------- 5. Font / text rendering ---------- */

static void font_init(const char *pattern) {
    if (FT_Init_FreeType(&g_ft_lib))
        die("FreeType init failed");

    if (!FcInit())
        die("fontconfig init failed");

    FcPattern *pat = FcNameParse((const FcChar8 *)pattern);
    if (!pat) die("invalid font pattern: %s", pattern);

    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult result;
    FcPattern *match = FcFontMatch(NULL, pat, &result);
    if (!match) die("no font matches pattern: %s", pattern);

    FcChar8 *file = NULL;
    int      index = 0;
    double   size  = DEFAULT_FONT_PIXEL;

    if (FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch)
        die("font match has no file");
    FcPatternGetInteger(match, FC_INDEX, 0, &index);
    FcPatternGetDouble(match, FC_PIXEL_SIZE, 0, &size);

    if (FT_New_Face(g_ft_lib, (const char *)file, index, &g_ft_face))
        die("cannot open font file: %s", file);

    if (FT_Set_Pixel_Sizes(g_ft_face, 0, (FT_UInt)(size + 0.5)))
        die("cannot set pixel size %d", (int)(size + 0.5));

    g_line_height = g_ft_face->size->metrics.height  >> 6;
    g_ascent      = g_ft_face->size->metrics.ascender >> 6;

    FcPatternDestroy(match);
    FcPatternDestroy(pat);
}

static void font_cleanup(void) {
    if (g_ft_face) FT_Done_Face(g_ft_face);
    if (g_ft_lib)  FT_Done_FreeType(g_ft_lib);
    FcFini();
}

/* Pixel width of a UTF-8 substring of length `n`. */
static int text_width_n(const char *s, size_t n) {
    int x = 0;
    uint32_t cp;
    size_t i = 0;
    while (i < n) {
        int k = utf8_decode_n(s + i, n - i, &cp);
        if (k == 0) { ++i; continue; }
        if (FT_Load_Char(g_ft_face, cp, FT_LOAD_DEFAULT) == 0)
            x += g_ft_face->glyph->advance.x >> 6;
        i += (size_t)k;
    }
    return x;
}

static void blit_glyph(uint32_t *pixels, int w, int h,
                       int pen_x, int pen_y, uint32_t color) {
    FT_GlyphSlot g = g_ft_face->glyph;
    FT_Bitmap   *bm = &g->bitmap;

    int x0 = pen_x + g->bitmap_left;
    int y0 = pen_y - g->bitmap_top;

    int fr = (color >> 16) & 0xFF;
    int fg = (color >>  8) & 0xFF;
    int fb =  color        & 0xFF;
    int fa = (color >> 24) & 0xFF;

    for (unsigned int j = 0; j < bm->rows; ++j) {
        int y = y0 + (int)j;
        if (y < 0 || y >= h) continue;
        for (unsigned int i = 0; i < bm->width; ++i) {
            int x = x0 + (int)i;
            if (x < 0 || x >= w) continue;

            unsigned char cov = bm->buffer[j * bm->pitch + i];
            if (cov == 0) continue;

            uint32_t alpha = (uint32_t)cov * (uint32_t)fa / 255u;
            if (alpha == 0) continue;

            uint32_t dst = pixels[y * w + x];
            uint32_t da  = (dst >> 24) & 0xFF;
            uint32_t dr  = (dst >> 16) & 0xFF;
            uint32_t dg  = (dst >>  8) & 0xFF;
            uint32_t db  =  dst        & 0xFF;

            uint32_t ia = 255u - alpha;

            uint32_t out_a = alpha + (da * ia) / 255u;
            uint32_t out_r = ((uint32_t)fr * alpha + dr * ia) / 255u;
            uint32_t out_g = ((uint32_t)fg * alpha + dg * ia) / 255u;
            uint32_t out_b = ((uint32_t)fb * alpha + db * ia) / 255u;

            pixels[y * w + x] = (out_a << 24) | (out_r << 16) |
                                (out_g <<  8) |  out_b;
        }
    }
}

/* Draw the first `n` bytes of `s` starting at (pen_x, baseline_y).
 * Returns horizontal advance in pixels. */
static int draw_text_n(uint32_t *pixels, int w, int h,
                       int pen_x, int baseline_y,
                       const char *s, size_t n, uint32_t color) {
    int x = pen_x;
    uint32_t cp;
    size_t i = 0;
    while (i < n) {
        int k = utf8_decode_n(s + i, n - i, &cp);
        if (k == 0) { ++i; continue; }
        if (FT_Load_Char(g_ft_face, cp, FT_LOAD_RENDER) == 0) {
            blit_glyph(pixels, w, h, x, baseline_y, color);
            x += g_ft_face->glyph->advance.x >> 6;
        }
        i += (size_t)k;
    }
    return x - pen_x;
}

/* ---------- 6. SHM buffer ---------- */

static int create_shm_fd(size_t size) {
    int fd = memfd_create("wnpt-shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Per-buffer state so we can munmap on release. wnpt redraws on every
 * keystroke, so unlike wlnch we MUST track buffer ownership: each
 * `wl_buffer.release` event needs to free its mmap and destroy the
 * buffer object, otherwise we leak one mapping per keystroke. */
struct buf_data {
    void  *mem;
    size_t size;
};

static void buffer_release(void *data, struct wl_buffer *buffer) {
    struct buf_data *bd = data;
    if (bd) {
        munmap(bd->mem, bd->size);
        free(bd);
    }
    wl_buffer_destroy(buffer);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static struct wl_buffer *create_shm_buffer(int w, int h, uint32_t **pixels_out) {
    int    stride = w * 4;
    size_t size   = (size_t)stride * (size_t)h;

    int fd = create_shm_fd(size);
    if (fd < 0) die("memfd_create failed: %s", strerror(errno));

    void *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) die("mmap failed: %s", strerror(errno));

    struct wl_shm_pool *pool = wl_shm_create_pool(g_shm, fd, (int32_t)size);
    struct wl_buffer   *buf  = wl_shm_pool_create_buffer(
        pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    struct buf_data *bd = malloc(sizeof(*bd));
    if (!bd) die("out of memory");
    bd->mem  = mem;
    bd->size = size;
    wl_buffer_add_listener(buf, &buffer_listener, bd);

    *pixels_out = (uint32_t *)mem;
    return buf;
}

/* ---------- 7. Drawing ---------- */

static void fill_rect(uint32_t *pixels, int w, int h, uint32_t color) {
    int n = w * h;
    for (int i = 0; i < n; ++i) pixels[i] = color;
}

/* Verbatim from wlnch.c. */
static void apply_rounded_corners(uint32_t *pixels, int w, int h, int radius) {
    if (radius <= 0) return;

    int r = radius;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    for (int j = 0; j < r; ++j) {
        for (int i = 0; i < r; ++i) {
            float dx = (float)r - 0.5f - (float)i;
            float dy = (float)r - 0.5f - (float)j;
            float dist = sqrtf(dx * dx + dy * dy);

            float coverage;
            if (dist <= (float)r - 0.5f)      coverage = 1.0f;
            else if (dist >= (float)r + 0.5f) coverage = 0.0f;
            else                              coverage = (float)r + 0.5f - dist;

            if (coverage >= 1.0f) continue;

            int xs[4] = { i,         w - 1 - i, i,         w - 1 - i };
            int ys[4] = { j,         j,         h - 1 - j, h - 1 - j };

            for (int k = 0; k < 4; ++k) {
                int x = xs[k], y = ys[k];
                if (coverage <= 0.0f) {
                    pixels[y * w + x] = 0x00000000U;
                } else {
                    uint32_t p  = pixels[y * w + x];
                    uint32_t a  = (p >> 24) & 0xFFu;
                    uint32_t na = (uint32_t)((float)a * coverage + 0.5f);
                    pixels[y * w + x] = (na << 24) | (p & 0x00FFFFFFu);
                }
            }
        }
    }
}

/* Walk g_text once to count lines and find the widest line in pixels. */
static void measure_text(int *out_max_line_w, int *out_n_lines) {
    int    n_lines    = 1;
    int    max_line_w = 0;
    size_t line_start = 0;
    for (size_t i = 0; i <= g_text_len; ++i) {
        bool is_eol = (i == g_text_len) || (g_text[i] == '\n');
        if (!is_eol) continue;

        int lw = text_width_n(g_text + line_start, i - line_start);
        if (lw > max_line_w) max_line_w = lw;

        if (i < g_text_len) {
            ++n_lines;
            line_start = i + 1;
        }
    }
    *out_max_line_w = max_line_w;
    *out_n_lines    = n_lines;
}

static void compute_window_size(int *out_w, int *out_h) {
    int row_h = g_line_height + ROW_GAP;

    int max_line_w, n_lines;
    measure_text(&max_line_w, &n_lines);

    /* Reserve a few pixels so the cursor at end-of-line isn't clipped
     * against the right padding. */
    int w = max_line_w + 2 * PADDING_X + CURSOR_WIDTH + 4;
    if (w < WNPT_MIN_WIDTH) w = WNPT_MIN_WIDTH;
    if (w > WNPT_MAX_WIDTH) w = WNPT_MAX_WIDTH;

    int h = 2 * PADDING_Y + n_lines * row_h - ROW_GAP;

    if (w & 1) ++w;
    if (h & 1) ++h;
    *out_w = w;
    *out_h = h;
}

static void render_frame(uint32_t *pixels, int w, int h) {
    fill_rect(pixels, w, h, COLOR_BG);

    int row_h = g_line_height + ROW_GAP;
    int y     = PADDING_Y + g_ascent;

    /* Walk the buffer and draw one segment per '\n'-delimited line.
     * Track the right edge of the *last* segment we drew so we know
     * where to plant the cursor. */
    size_t line_start = 0;
    int    cursor_x   = PADDING_X;
    int    cursor_y   = y - g_ascent;

    for (size_t i = 0; i <= g_text_len; ++i) {
        bool is_eol = (i == g_text_len) || (g_text[i] == '\n');
        if (!is_eol) continue;

        int adv = draw_text_n(pixels, w, h,
                              PADDING_X, y,
                              g_text + line_start, i - line_start,
                              COLOR_FG);

        cursor_x = PADDING_X + adv;
        cursor_y = y - g_ascent;

        if (i < g_text_len) {
            line_start = i + 1;
            y += row_h;
        }
    }

    /* Solid vertical bar cursor at the end of the last line. */
    for (int j = 0; j < g_line_height; ++j) {
        int py = cursor_y + j;
        if (py < 0 || py >= h) continue;
        for (int i = 0; i < CURSOR_WIDTH; ++i) {
            int px = cursor_x + i;
            if (px < 0 || px >= w) continue;
            pixels[py * w + px] = CURSOR_COLOR;
        }
    }

    apply_rounded_corners(pixels, w, h, CORNER_RADIUS);
}

static void draw_and_attach(void) {
    uint32_t *pixels = NULL;
    struct wl_buffer *buf = create_shm_buffer(g_win_w, g_win_h, &pixels);

    render_frame(pixels, g_win_w, g_win_h);

    wl_surface_attach(g_surface, buf, 0, 0);
    wl_surface_damage_buffer(g_surface, 0, 0, g_win_w, g_win_h);
    wl_surface_commit(g_surface);
}

/* Re-measure the buffer; if the window needs to grow or shrink, ask the
 * compositor to resize via set_size+commit (the next configure event
 * triggers the redraw). Otherwise just attach a fresh buffer of the
 * current size. Called after every keystroke that might change layout. */
static void schedule_redraw(void) {
    int new_w, new_h;
    compute_window_size(&new_w, &new_h);

    if (new_w != g_win_w || new_h != g_win_h) {
        g_win_w = new_w;
        g_win_h = new_h;
        zwlr_layer_surface_v1_set_size(
            g_layer_surface, (uint32_t)new_w, (uint32_t)new_h);
        wl_surface_commit(g_surface);
        /* draw on the configure that follows */
    } else {
        draw_and_attach();
    }
}

/* ---------- 8. Wayland listeners ---------- */

/* --- layer surface --- */

static void layer_surface_configure(void *data,
                                    struct zwlr_layer_surface_v1 *s,
                                    uint32_t serial,
                                    uint32_t width, uint32_t height) {
    (void)data; (void)width; (void)height;
    zwlr_layer_surface_v1_ack_configure(s, serial);
    /* Always redraw on configure: covers both the initial surface
     * map and any resize triggered by schedule_redraw. */
    draw_and_attach();
}

static void layer_surface_closed(void *data,
                                 struct zwlr_layer_surface_v1 *s) {
    (void)data; (void)s;
    g_running = 0;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed    = layer_surface_closed,
};

/* --- keyboard --- */

static void keyboard_keymap(void *data, struct wl_keyboard *kb,
                            uint32_t format, int32_t fd, uint32_t size) {
    (void)data; (void)kb;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }

    char *map_str = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map_str == MAP_FAILED) {
        close(fd);
        return;
    }

    struct xkb_keymap *km = xkb_keymap_new_from_string(
        g_xkb_ctx, map_str, XKB_KEYMAP_FORMAT_TEXT_V1, 0);
    munmap(map_str, size);
    close(fd);
    if (!km) return;

    struct xkb_state *st = xkb_state_new(km);
    if (!st) {
        xkb_keymap_unref(km);
        return;
    }

    if (g_xkb_state)  xkb_state_unref(g_xkb_state);
    if (g_xkb_keymap) xkb_keymap_unref(g_xkb_keymap);
    g_xkb_keymap = km;
    g_xkb_state  = st;
}

static void keyboard_enter(void *d, struct wl_keyboard *kb, uint32_t serial,
                           struct wl_surface *s, struct wl_array *keys) {
    (void)d; (void)kb; (void)serial; (void)s; (void)keys;
}

static void keyboard_leave(void *d, struct wl_keyboard *kb, uint32_t serial,
                           struct wl_surface *s) {
    (void)d; (void)kb; (void)serial; (void)s;
}

static void keyboard_key(void *data, struct wl_keyboard *kb,
                         uint32_t serial, uint32_t time,
                         uint32_t key, uint32_t state) {
    (void)data; (void)kb; (void)serial; (void)time;

    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) return;
    if (!g_xkb_state || !g_xkb_keymap) return;

    /* Wayland reports evdev keycodes; xkb expects evdev + 8. */
    xkb_keycode_t keycode = key + 8;

    /* Unlike wlnch we honour the user's *current* layout: this is a
     * text input prompt, so Cyrillic / Greek / Dvorak should produce
     * the corresponding glyphs. */
    xkb_keysym_t sym = xkb_state_key_get_one_sym(g_xkb_state, keycode);

    int ctrl_active = xkb_state_mod_name_is_active(
        g_xkb_state, XKB_MOD_NAME_CTRL,  XKB_STATE_MODS_EFFECTIVE) > 0;
    int shift_active = xkb_state_mod_name_is_active(
        g_xkb_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0;
    int alt_active = xkb_state_mod_name_is_active(
        g_xkb_state, XKB_MOD_NAME_ALT,   XKB_STATE_MODS_EFFECTIVE) > 0;
    int super_active = xkb_state_mod_name_is_active(
        g_xkb_state, XKB_MOD_NAME_LOGO,  XKB_STATE_MODS_EFFECTIVE) > 0;

    /* Cancel without emitting anything. */
    if (sym == XKB_KEY_Escape ||
        (ctrl_active && (sym == XKB_KEY_g || sym == XKB_KEY_G))) {
        g_committed = 0;
        g_running   = 0;
        return;
    }

    /* Enter without Shift commits; Shift+Enter inserts a newline. */
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        if (shift_active) {
            buf_append("\n", 1);
            schedule_redraw();
        } else {
            g_committed = 1;
            g_running   = 0;
        }
        return;
    }

    /* Backspace: delete previous codepoint (or word with Ctrl). */
    if (sym == XKB_KEY_BackSpace) {
        if (ctrl_active) buf_pop_word();
        else             buf_pop_codepoint();
        schedule_redraw();
        return;
    }

    /* Ctrl+U clears the buffer (readline-style "kill line"). */
    if (ctrl_active && !shift_active && !alt_active && !super_active &&
        (sym == XKB_KEY_u || sym == XKB_KEY_U)) {
        buf_clear();
        schedule_redraw();
        return;
    }

    /* Ignore anything else with Ctrl / Alt / Super held; we don't want
     * Ctrl+letter or Alt+letter to leak as text into the buffer. */
    if (ctrl_active || alt_active || super_active) return;

    /* Otherwise insert the UTF-8 the keypress would produce, taking
     * Shift / CapsLock / dead keys / etc. into account. */
    char utf8[16];
    int n = xkb_state_key_get_utf8(g_xkb_state, keycode,
                                   utf8, sizeof(utf8));
    if (n <= 0) return;

    /* Filter ASCII control characters (other than '\t'). Newlines are
     * handled above via the Enter keysym path. */
    if (n == 1) {
        unsigned char c = (unsigned char)utf8[0];
        if (c < 0x20 && c != '\t') return;
        if (c == 0x7F) return;
    }

    buf_append(utf8, (size_t)n);
    schedule_redraw();
}

static void keyboard_modifiers(void *data, struct wl_keyboard *kb,
                               uint32_t serial,
                               uint32_t mods_depressed, uint32_t mods_latched,
                               uint32_t mods_locked, uint32_t group) {
    (void)data; (void)kb; (void)serial;
    if (!g_xkb_state) return;
    xkb_state_update_mask(g_xkb_state,
                          mods_depressed, mods_latched, mods_locked,
                          0, 0, group);
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *kb,
                                 int32_t rate, int32_t delay) {
    (void)data; (void)kb; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap      = keyboard_keymap,
    .enter       = keyboard_enter,
    .leave       = keyboard_leave,
    .key         = keyboard_key,
    .modifiers   = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

/* --- seat --- */

static void seat_capabilities(void *data, struct wl_seat *seat,
                              uint32_t caps) {
    (void)data;
    int has_kb = !!(caps & WL_SEAT_CAPABILITY_KEYBOARD);
    if (has_kb && !g_keyboard) {
        g_keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(g_keyboard, &keyboard_listener, NULL);
    } else if (!has_kb && g_keyboard) {
        wl_keyboard_release(g_keyboard);
        g_keyboard = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *s, const char *name) {
    (void)data; (void)s; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name         = seat_name,
};

/* --- registry --- */

static void registry_global(void *data, struct wl_registry *r, uint32_t name,
                            const char *iface, uint32_t version) {
    (void)data;
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        uint32_t v = version > 4 ? 4 : version;
        g_compositor = wl_registry_bind(r, name, &wl_compositor_interface, v);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        g_shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, wl_seat_interface.name) == 0) {
        uint32_t v = version > 5 ? 5 : version;
        g_seat = wl_registry_bind(r, name, &wl_seat_interface, v);
        wl_seat_add_listener(g_seat, &seat_listener, NULL);
    } else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
        uint32_t v = version > 4 ? 4 : version;
        g_layer_shell = wl_registry_bind(
            r, name, &zwlr_layer_shell_v1_interface, v);
    }
}

static void registry_global_remove(void *data, struct wl_registry *r,
                                   uint32_t name) {
    (void)data; (void)r; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

/* ---------- 9. main ---------- */

static void usage(void) {
    fputs(
        "usage: wnpt [-f FONT]\n"
        "  Reads typed text from a Wayland overlay window. On Enter,\n"
        "  prints the buffered text to stdout and exits 0. On Esc or\n"
        "  Ctrl-G, exits 1 without printing anything.\n"
        "  Shift+Enter inserts a newline. Backspace deletes the last\n"
        "  codepoint; Ctrl+Backspace deletes the last word; Ctrl+U\n"
        "  clears the buffer.\n"
        "\n"
        "  -f FONT   fontconfig pattern (env: WNPT_FONT, then WLNCH_FONT)\n",
        stderr);
}

int main(int argc, char **argv) {
    int opt;
    while ((opt = getopt(argc, argv, "f:h")) != -1) {
        switch (opt) {
        case 'f': g_font_pattern = optarg; break;
        case 'h': usage(); return 0;
        default:  usage(); return 2;
        }
    }
    if (argc - optind != 0) {
        usage();
        return 2;
    }

    if (!g_font_pattern) {
        const char *env = getenv("WNPT_FONT");
        if (!env || !*env) env = getenv("WLNCH_FONT");
        g_font_pattern = (env && *env) ? env : DEFAULT_FONT;
    }

    font_init(g_font_pattern);

    /* Initial window size: empty buffer, one line tall, min width. */
    compute_window_size(&g_win_w, &g_win_h);

    g_xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!g_xkb_ctx) die("xkb_context_new failed");

    g_display = wl_display_connect(NULL);
    if (!g_display) die("cannot connect to Wayland display");

    g_registry = wl_display_get_registry(g_display);
    wl_registry_add_listener(g_registry, &registry_listener, NULL);
    wl_display_roundtrip(g_display);

    if (!g_compositor) die("compositor missing wl_compositor");
    if (!g_shm)        die("compositor missing wl_shm");
    if (!g_seat)       die("compositor missing wl_seat");
    if (!g_layer_shell)
        die("compositor does not support wlr-layer-shell-unstable-v1\n"
            "       (this includes GNOME/Mutter; wnpt needs Sway, "
            "Hyprland, KDE, etc.)");

    /* Second roundtrip so seat capability events arrive. */
    wl_display_roundtrip(g_display);

    g_surface = wl_compositor_create_surface(g_compositor);
    g_layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        g_layer_shell, g_surface, NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "wnpt");

    zwlr_layer_surface_v1_add_listener(
        g_layer_surface, &layer_surface_listener, NULL);
    zwlr_layer_surface_v1_set_size(
        g_layer_surface, (uint32_t)g_win_w, (uint32_t)g_win_h);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        g_layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    /* No anchors -> centered. */

    wl_surface_commit(g_surface);

    while (g_running && wl_display_dispatch(g_display) != -1) {
        /* nothing */
    }

    /* Commit the buffer to stdout before tearing down the compositor
     * connection: keeps the spawn-and-pipe pattern (`wnpt > note.txt`)
     * predictable. A trailing newline is appended if missing so the
     * file ends well-formed; an empty buffer prints nothing at all. */
    if (g_committed && g_text_len > 0) {
        fwrite(g_text, 1, g_text_len, stdout);
        if (g_text[g_text_len - 1] != '\n')
            fputc('\n', stdout);
        fflush(stdout);
    }

    if (g_layer_surface) zwlr_layer_surface_v1_destroy(g_layer_surface);
    if (g_surface)       wl_surface_destroy(g_surface);
    if (g_keyboard)      wl_keyboard_release(g_keyboard);
    if (g_seat)          wl_seat_destroy(g_seat);
    if (g_layer_shell)   zwlr_layer_shell_v1_destroy(g_layer_shell);
    if (g_shm)           wl_shm_destroy(g_shm);
    if (g_compositor)    wl_compositor_destroy(g_compositor);
    if (g_registry)      wl_registry_destroy(g_registry);
    if (g_display) {
        wl_display_flush(g_display);
        wl_display_disconnect(g_display);
    }

    if (g_xkb_state)  xkb_state_unref(g_xkb_state);
    if (g_xkb_keymap) xkb_keymap_unref(g_xkb_keymap);
    if (g_xkb_ctx)    xkb_context_unref(g_xkb_ctx);

    font_cleanup();

    free(g_text);

    /* Exit code mirrors commit/cancel: 0 if the user pressed Enter,
     * 1 otherwise (Esc / Ctrl-G / surface-closed). */
    return g_committed ? 0 : 1;
}
