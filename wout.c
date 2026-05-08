/*
 * wout - tiny Wayland stdin viewer, companion to wlnch / wnpt.
 *
 * Reads all of stdin into memory at startup, opens an OVERLAY
 * layer-shell window, grabs the keyboard exclusively, renders the
 * text statically (one '\n'-delimited line per row), and exits when
 * the user dismisses the window. There is no editing, no cursor,
 * and no scrolling — it's strictly a "show this" dialog.
 *
 * Dismiss keys (any of):
 *
 *   Esc, Enter, q, Space, Ctrl+G
 *
 * An optional `-t MS` / `--timeout MS` flag auto-closes the window
 * after MS milliseconds. The default of 0 means "no timeout"; the
 * window stays open until the user dismisses it.
 *
 * Typical usage:
 *
 *   echo "hello"            | wout
 *   git log --oneline -10   | wout
 *   date                    | wout
 *   echo "build finished"   | wout -t 3000   # auto-close after 3 s
 *
 * Visual styling (font, colors, padding, corner radius) is shared
 * with wlnch / wnpt via config.h. Sizing is bounded by
 * WOUT_MIN_WIDTH / WOUT_MAX_WIDTH / WOUT_MAX_HEIGHT — long lines
 * clip at the right edge, extra rows clip at the bottom.
 *
 * Single translation unit, mostly verbatim from wlnch.c / wnpt.c
 * by design — the three tools are siblings, not a library. Layout:
 *
 *   1. Includes
 *   2. Globals
 *   3. Utility helpers (UTF-8, OOM, stdin slurp)
 *   4. Font / text rendering
 *   5. SHM buffer
 *   6. Drawing
 *   7. Wayland listeners
 *   8. main()
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <fontconfig/fontconfig.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include "config.h"

/* ---------- 2. Globals ---------- */

/* Slurped stdin contents. Always nul-terminated. Trailing '\n' /
 * '\r' are stripped so a typical `printf "foo\n" | wout` doesn't
 * grow the window with a useless empty bottom row. */
static char   *g_text;
static size_t  g_text_len;

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

static int                            g_running         = 1;
static int                            g_buffer_attached = 0;
static int                            g_win_w           = 0;
static int                            g_win_h           = 0;

static const char *g_font_pattern = NULL;

/* Auto-close timeout in milliseconds. 0 means "no timeout"; the
 * window stays open until the user dismisses it. Set by `-t MS` /
 * `--timeout MS`. The countdown starts when we enter the dispatch
 * loop, just before we begin servicing Wayland events. */
static long g_timeout_ms = 0;

/* ---------- 3. Utility helpers ---------- */

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "wout: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

/* Bounded UTF-8 decode (copied from wnpt.c). Returns bytes consumed
 * or 0 on error. Stops at `s + max` even mid-sequence. */
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

/* Parse a non-negative integer (millisecond count) from a CLI arg.
 * Rejects empty, negative, non-numeric, or out-of-range values. */
static long parse_ms(const char *s) {
    if (!s || !*s)
        die("invalid timeout: empty value");
    errno = 0;
    char *end;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0')
        die("invalid timeout '%s': expected integer milliseconds", s);
    if (v < 0)
        die("invalid timeout '%s': must be >= 0 (0 means no timeout)", s);
    if (v > INT_MAX)
        die("invalid timeout '%s': too large (max %d ms)", s, INT_MAX);
    return v;
}

/* Slurp all of stdin into a freshly-allocated nul-terminated buffer.
 * Strips trailing newline / CR bytes so the rendered window doesn't
 * grow an empty bottom row from `echo`'s implicit `\n`. */
static void slurp_stdin(void) {
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) die("out of memory");

    for (;;) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *p = realloc(buf, cap);
            if (!p) die("out of memory");
            buf = p;
        }
        size_t n = fread(buf + len, 1, cap - len - 1, stdin);
        len += n;
        if (n == 0) {
            if (ferror(stdin)) die("read failed: %s", strerror(errno));
            break;
        }
    }

    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        --len;

    buf[len]   = '\0';
    g_text     = buf;
    g_text_len = len;
}

/* ---------- 4. Font / text rendering ---------- */

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

/* ---------- 5. SHM buffer ---------- */

static int create_shm_fd(size_t size) {
    int fd = memfd_create("wout-shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

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

    *pixels_out = (uint32_t *)mem;
    return buf;
}

/* ---------- 6. Drawing ---------- */

static void fill_rect(uint32_t *pixels, int w, int h, uint32_t color) {
    int n = w * h;
    for (int i = 0; i < n; ++i) pixels[i] = color;
}

/* Verbatim from wlnch.c / wnpt.c. */
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

static void compute_window_size(void) {
    int row_h = g_line_height + ROW_GAP;

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

    int w = max_line_w + 2 * PADDING_X;
    if (w < WOUT_MIN_WIDTH) w = WOUT_MIN_WIDTH;
    if (w > WOUT_MAX_WIDTH) w = WOUT_MAX_WIDTH;

    int h = 2 * PADDING_Y + n_lines * row_h - ROW_GAP;
    if (h > WOUT_MAX_HEIGHT) h = WOUT_MAX_HEIGHT;

    if (w & 1) ++w;
    if (h & 1) ++h;
    g_win_w = w;
    g_win_h = h;
}

static void render_frame(uint32_t *pixels, int w, int h) {
    fill_rect(pixels, w, h, COLOR_BG);

    int row_h = g_line_height + ROW_GAP;
    int y     = PADDING_Y + g_ascent;

    /* Walk the buffer one '\n'-delimited line at a time. blit_glyph
     * already clips against w/h, so lines past the right or bottom
     * edge are silently truncated by the corresponding WOUT_MAX_*. */
    size_t line_start = 0;
    for (size_t i = 0; i <= g_text_len; ++i) {
        bool is_eol = (i == g_text_len) || (g_text[i] == '\n');
        if (!is_eol) continue;

        draw_text_n(pixels, w, h,
                    PADDING_X, y,
                    g_text + line_start, i - line_start,
                    COLOR_FG);

        if (i < g_text_len) {
            line_start = i + 1;
            y += row_h;
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

    /* One-shot lifecycle, like wlnch: we never re-render, so the
     * single buffer is implicitly released when the connection
     * tears down on exit. */
    g_buffer_attached = 1;
    (void)buf;
}

/* ---------- 7. Wayland listeners ---------- */

/* --- layer surface --- */

static void layer_surface_configure(void *data,
                                    struct zwlr_layer_surface_v1 *s,
                                    uint32_t serial,
                                    uint32_t width, uint32_t height) {
    (void)data; (void)width; (void)height;
    zwlr_layer_surface_v1_ack_configure(s, serial);
    if (!g_buffer_attached) draw_and_attach();
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

    /* Resolve via layout 0 like wlnch, so the dismissal `q` works
     * even when the user is on a non-Latin layout. */
    xkb_keycode_t      keycode = key + 8;
    xkb_layout_index_t layout  = 0;
    xkb_level_index_t  level   = xkb_state_key_get_level(
        g_xkb_state, keycode, layout);
    const xkb_keysym_t *syms = NULL;
    int n_syms = xkb_keymap_key_get_syms_by_level(
        g_xkb_keymap, keycode, layout, level, &syms);
    xkb_keysym_t sym = (n_syms > 0) ? syms[0] : XKB_KEY_NoSymbol;

    bool ctrl = xkb_state_mod_name_is_active(
        g_xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0;

    /* Any of the standard "dismiss" keys closes the window. We
     * include Enter / Space so wout works as a "press any key to
     * continue" overlay, plus the wlnch-style Esc / q / Ctrl-G. */
    if (sym == XKB_KEY_Escape ||
        sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter ||
        sym == XKB_KEY_space  ||
        sym == XKB_KEY_q      || sym == XKB_KEY_Q ||
        (ctrl && (sym == XKB_KEY_g || sym == XKB_KEY_G))) {
        g_running = 0;
    }
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

/* ---------- 8. main ---------- */

static void usage(void) {
    fputs(
        "usage: wout [-f FONT] [-t MS]\n"
        "  Reads stdin into memory and shows it in a Wayland overlay.\n"
        "  Dismiss with Esc, Enter, q, Space, or Ctrl+G.\n"
        "\n"
        "  Long lines clip at the right edge (WOUT_MAX_WIDTH); rows past\n"
        "  WOUT_MAX_HEIGHT clip at the bottom — pipe through `head` /\n"
        "  `cut` if you only want the start of a long file.\n"
        "\n"
        "  -f, --font FONT       fontconfig pattern\n"
        "                        (env: WOUT_FONT, then WLNCH_FONT)\n"
        "  -t, --timeout MS      auto-close after MS milliseconds;\n"
        "                        0 means no timeout (default)\n"
        "  -h, --help            show this help and exit\n"
        "\n"
        "  Examples:\n"
        "    echo hello              | wout\n"
        "    git log --oneline       | wout\n"
        "    date                    | wout -t 3000\n",
        stderr);
}

static const struct option long_options[] = {
    { "font",    required_argument, NULL, 'f' },
    { "timeout", required_argument, NULL, 't' },
    { "help",    no_argument,       NULL, 'h' },
    { NULL,      0,                 NULL, 0   },
};

int main(int argc, char **argv) {
    int opt;
    while ((opt = getopt_long(argc, argv, "f:t:h",
                              long_options, NULL)) != -1) {
        switch (opt) {
        case 'f': g_font_pattern = optarg;          break;
        case 't': g_timeout_ms   = parse_ms(optarg); break;
        case 'h': usage(); return 0;
        default:  usage(); return 2;
        }
    }
    if (argc - optind != 0) {
        usage();
        return 2;
    }

    /* Refuse to read from a tty: the whole point of wout is to
     * display piped output. Without this guard a bare `wout` would
     * silently block on stdin from the terminal, which looks like
     * a hang. */
    if (isatty(STDIN_FILENO))
        die("stdin must be a pipe or redirection, e.g. `echo hi | wout`");

    if (!g_font_pattern) {
        const char *env = getenv("WOUT_FONT");
        if (!env || !*env) env = getenv("WLNCH_FONT");
        g_font_pattern = (env && *env) ? env : DEFAULT_FONT;
    }

    slurp_stdin();

    font_init(g_font_pattern);
    compute_window_size();

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
            "       (this includes GNOME/Mutter; wout needs Sway, "
            "Hyprland, KDE, etc.)");

    /* Second roundtrip so seat capability events arrive. */
    wl_display_roundtrip(g_display);

    g_surface = wl_compositor_create_surface(g_compositor);
    g_layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        g_layer_shell, g_surface, NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "wout");

    zwlr_layer_surface_v1_add_listener(
        g_layer_surface, &layer_surface_listener, NULL);
    zwlr_layer_surface_v1_set_size(
        g_layer_surface, (uint32_t)g_win_w, (uint32_t)g_win_h);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        g_layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    /* No anchors -> centered. */

    wl_surface_commit(g_surface);

    /* Dispatch loop with optional timeout. We can't use the simpler
     * `wl_display_dispatch` because it blocks indefinitely; instead
     * we run the standard prepare_read / poll / read_events cycle
     * with `poll(2)`'s timeout argument set to the remaining time
     * until g_timeout_ms elapses (or -1 for "no timeout"). */
    struct timespec start_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    while (g_running) {
        /* Compute remaining timeout in milliseconds, or -1 for none. */
        int wait_ms = -1;
        if (g_timeout_ms > 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed = (now.tv_sec  - start_ts.tv_sec)  * 1000L +
                           (now.tv_nsec - start_ts.tv_nsec) / 1000000L;
            long remaining = g_timeout_ms - elapsed;
            if (remaining <= 0) break;
            wait_ms = (remaining > INT_MAX) ? INT_MAX : (int)remaining;
        }

        /* Drain any events the library has already queued, then arm
         * the read for the actual fd-poll. */
        while (wl_display_prepare_read(g_display) != 0) {
            if (wl_display_dispatch_pending(g_display) < 0)
                die("wl_display_dispatch_pending: %s", strerror(errno));
        }
        if (wl_display_flush(g_display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(g_display);
            die("wl_display_flush: %s", strerror(errno));
        }

        struct pollfd pfd = {
            .fd     = wl_display_get_fd(g_display),
            .events = POLLIN,
        };
        int n = poll(&pfd, 1, wait_ms);
        if (n < 0) {
            wl_display_cancel_read(g_display);
            if (errno == EINTR) continue;
            die("poll: %s", strerror(errno));
        }
        if (n == 0) {
            /* Timed out — close cleanly. */
            wl_display_cancel_read(g_display);
            break;
        }
        if (!(pfd.revents & POLLIN)) {
            wl_display_cancel_read(g_display);
            /* POLLERR / POLLHUP on the display fd: compositor went away. */
            break;
        }
        if (wl_display_read_events(g_display) < 0)
            die("wl_display_read_events: %s", strerror(errno));
        if (wl_display_dispatch_pending(g_display) < 0)
            die("wl_display_dispatch_pending: %s", strerror(errno));
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

    return 0;
}
