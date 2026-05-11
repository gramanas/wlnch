/*
 * wnpt - tiny Wayland note prompt, companion to wlnch.
 *
 * Opens an OVERLAY layer-shell window, grabs the keyboard exclusively,
 * and lets the user type arbitrary UTF-8 text with readline-style
 * editing. The cursor lives at an arbitrary byte offset inside the
 * buffer (not just the end), and a single-slot kill ring with
 * consecutive-kill accumulation supports yank.
 *
 * Submission:
 *
 *   Enter           commit: print the buffer to stdout and exit 0.
 *   Shift+Enter     insert a newline into the buffer.
 *   Esc / Ctrl+G    abort: exit 1, nothing is written to stdout.
 *
 * An optional prompt string can be displayed at the start of the
 * first row via `-p PROMPT`. The prompt is purely visual: it never
 * appears in the buffer and is never written to stdout, only the
 * typed text is.
 *
 * Cursor movement:
 *
 *   Ctrl+B / Left           one char back
 *   Ctrl+F / Right          one char forward
 *   Alt+B  / Ctrl+Left      one word back
 *   Alt+F  / Ctrl+Right     one word forward
 *   Ctrl+A / Home           beginning of current line
 *   Ctrl+E / End            end of current line
 *   Up / Down               previous / next line, same column
 *
 * Editing:
 *
 *   Backspace / Ctrl+H      delete previous char
 *   Delete    / Ctrl+D      delete next char
 *   Ctrl+W / Ctrl+Backspace / Alt+Backspace
 *                           kill previous word (into kill ring)
 *   Alt+D                   kill next word (into kill ring)
 *   Ctrl+K                  kill to end of line
 *   Ctrl+U                  kill to beginning of line
 *   Ctrl+Y                  yank (paste) the kill ring at point
 *   Ctrl+T                  transpose the two chars around point
 *
 * Pasting from the system clipboards:
 *
 *   Ctrl+V                  paste the Wayland clipboard selection
 *                           (wl_data_device, what Ctrl+C/Ctrl+V apps
 *                           write to)
 *   Shift+Insert            paste the primary selection
 *                           (zwp_primary_selection_device_v1, what
 *                           middle-click pastes from)
 *
 *   Pasted text is inserted at the cursor; embedded newlines stay
 *   in the buffer and do *not* commit (Enter is for commit).
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
 *   2. Globals (text buffer, cursor, kill ring, wayland, xkb, font,
 *               clipboards)
 *   3. Utility helpers (UTF-8, OOM)
 *   4. Text buffer + cursor + kill ring
 *   5. Font / text rendering
 *   6. SHM buffer with release tracking
 *   7. Drawing
 *   8. Clipboard / primary selection paste
 *   9. Wayland listeners
 *  10. main()
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
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
#include <unistd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <fontconfig/fontconfig.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "primary-selection-unstable-v1-client-protocol.h"

#include "config.h"

/* ---------- 2. Globals ---------- */

/* The typed text. Always nul-terminated when len > 0; cap > len + 1.
 * Newlines in the buffer are real '\n' bytes inserted by Shift+Enter.
 * `g_cursor` is a byte offset in [0, g_text_len]; insertions and
 * deletions act relative to it. */
static char   *g_text;
static size_t  g_text_len;
static size_t  g_text_cap;
static size_t  g_cursor;

/* Single-slot kill ring. Consecutive kill commands accumulate into
 * the same buffer (forward kills append, backward kills prepend), so
 * a sequence like Ctrl+K Ctrl+K Ctrl+Y restores everything as one
 * paste. `g_last_was_kill` is reset at the top of every key handler
 * and re-asserted by each kill_*() helper. */
static char   *g_kill;
static size_t  g_kill_len;
static size_t  g_kill_cap;
static bool    g_last_was_kill;

/* Optional `-p PROMPT` string shown at the start of the first row,
 * in COLOR_PROMPT. Purely visual — it never enters g_text and is
 * never printed on commit. `g_prompt_w` is the prompt's pixel
 * width, cached after font init. The prompt is single-line: '\n'
 * is rejected at parse time. */
static const char *g_prompt;
static size_t      g_prompt_len;
static int         g_prompt_w;

static struct wl_display            * g_display;
static struct wl_registry           * g_registry;
static struct wl_compositor         * g_compositor;
static struct wl_shm                * g_shm;
static struct wl_seat               * g_seat;
static struct wl_keyboard           * g_keyboard;
static struct zwlr_layer_shell_v1   * g_layer_shell;
static struct wl_surface            * g_surface;
static struct zwlr_layer_surface_v1 * g_layer_surface;

/* Clipboard (Ctrl+V) and primary selection (Shift+Insert).
 *
 * For each device we keep one outstanding offer at a time: the
 * `selection` event delivers a fresh wl_data_offer and we destroy
 * the previous one. Each offer carries a `paste_offer_state` (set
 * via set_user_data) that accumulates the MIME types the source
 * advertises, so that on paste we can pick the most preferred text
 * MIME without re-querying the server.
 *
 * Either manager may be missing on minimal compositors; the matching
 * paste binding then no-ops silently. */
struct paste_offer_state {
    bool has_utf8;        /* text/plain;charset=utf-8 (preferred) */
    bool has_utf8_string; /* UTF8_STRING (X11 legacy)              */
    bool has_plain;       /* text/plain (assumed UTF-8)            */
    bool has_string;      /* STRING / TEXT (best-effort UTF-8)     */
};

static struct wl_data_device_manager                 * g_data_dev_mgr;
static struct wl_data_device                         * g_data_device;
static struct wl_data_offer                          * g_clipboard_offer;
static struct paste_offer_state                      * g_clipboard_offer_state;

static struct zwp_primary_selection_device_manager_v1 * g_primary_mgr;
static struct zwp_primary_selection_device_v1         * g_primary_device;
static struct zwp_primary_selection_offer_v1          * g_primary_offer;
static struct paste_offer_state                       * g_primary_offer_state;

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

/* ---------- 4. Text buffer + cursor + kill ring ---------- */

/* --- UTF-8 codepoint stepping inside g_text --- */

/* Byte offset of the start of the codepoint immediately before `pos`.
 * Walks backward over continuation bytes (10xxxxxx) then one start
 * byte. Returns 0 if `pos` is already at the buffer start. */
static size_t prev_codepoint(size_t pos) {
    if (pos == 0) return 0;
    --pos;
    while (pos > 0 && (((unsigned char)g_text[pos] & 0xC0) == 0x80))
        --pos;
    return pos;
}

/* Byte offset just past the codepoint that starts at `pos`. Walks
 * forward one byte then over continuation bytes. Returns g_text_len
 * if `pos` is at end-of-buffer. */
static size_t next_codepoint(size_t pos) {
    if (pos >= g_text_len) return g_text_len;
    ++pos;
    while (pos < g_text_len && (((unsigned char)g_text[pos] & 0xC0) == 0x80))
        ++pos;
    return pos;
}

/* Number of codepoints (not bytes) in g_text[start..end). Used as the
 * "column" coordinate for Up/Down line motion. */
static size_t codepoints_in(size_t start, size_t end) {
    size_t n = 0;
    while (start < end) {
        start = next_codepoint(start);
        ++n;
    }
    return n;
}

/* Advance from `start` by up to `n` codepoints, clamped at `end`. */
static size_t advance_codepoints(size_t start, size_t end, size_t n) {
    while (n > 0 && start < end) {
        size_t next = next_codepoint(start);
        if (next > end) break;
        start = next;
        --n;
    }
    return start;
}

/* --- Line bounds inside g_text (lines are '\n'-delimited) --- */

static size_t line_start_at(size_t pos) {
    while (pos > 0 && g_text[pos - 1] != '\n') --pos;
    return pos;
}

static size_t line_end_at(size_t pos) {
    while (pos < g_text_len && g_text[pos] != '\n') ++pos;
    return pos;
}

/* --- Word boundaries (readline-style) ---
 *
 * "Word char" = ASCII alphanumeric, underscore, or any byte >= 0x80
 * (so multi-byte UTF-8 characters and continuation bytes are all
 * treated as word). Everything else (spaces, ASCII punctuation,
 * control bytes) is a separator. */
static bool is_word_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_' ||
           c >= 0x80;
}

static size_t word_back(size_t pos) {
    while (pos > 0 && !is_word_byte((unsigned char)g_text[pos - 1])) --pos;
    while (pos > 0 &&  is_word_byte((unsigned char)g_text[pos - 1])) --pos;
    return pos;
}

static size_t word_forward(size_t pos) {
    while (pos < g_text_len &&
           !is_word_byte((unsigned char)g_text[pos])) ++pos;
    while (pos < g_text_len &&
            is_word_byte((unsigned char)g_text[pos])) ++pos;
    return pos;
}

/* --- Cursor movement (vertical) --- */

static void cursor_up(void) {
    size_t ls = line_start_at(g_cursor);
    if (ls == 0) return;
    size_t col = codepoints_in(ls, g_cursor);
    size_t prev_end   = ls - 1;            /* the '\n' that ended prev line */
    size_t prev_start = line_start_at(prev_end);
    g_cursor = advance_codepoints(prev_start, prev_end, col);
}

static void cursor_down(void) {
    size_t le = line_end_at(g_cursor);
    if (le == g_text_len) return;
    size_t ls  = line_start_at(g_cursor);
    size_t col = codepoints_in(ls, g_cursor);
    size_t next_start = le + 1;
    size_t next_end   = line_end_at(next_start);
    g_cursor = advance_codepoints(next_start, next_end, col);
}

/* --- Buffer mutation (cursor-aware) --- */

/* Insert `n` bytes from `s` at the current cursor position; advance
 * the cursor past the insertion. */
static void buf_insert(const char *s, size_t n) {
    if (n == 0) return;
    if (g_text_len + n + 1 > g_text_cap) {
        size_t newcap = g_text_cap ? g_text_cap * 2 : 64;
        while (newcap < g_text_len + n + 1) newcap *= 2;
        char *p = realloc(g_text, newcap);
        if (!p) die("out of memory");
        g_text     = p;
        g_text_cap = newcap;
    }
    /* +1 to also shift the trailing nul. */
    memmove(g_text + g_cursor + n,
            g_text + g_cursor,
            g_text_len - g_cursor + 1);
    memcpy(g_text + g_cursor, s, n);
    g_text_len += n;
    g_cursor   += n;
}

/* Delete bytes [start, end) from the buffer; adjust cursor. */
static void buf_delete_range(size_t start, size_t end) {
    if (end > g_text_len) end = g_text_len;
    if (start >= end)     return;
    /* +1 to also shift the trailing nul. */
    memmove(g_text + start, g_text + end, g_text_len - end + 1);
    size_t n = end - start;
    g_text_len -= n;
    if (g_cursor >= end)        g_cursor -= n;
    else if (g_cursor > start)  g_cursor  = start;
}

/* --- Kill ring --- */

static void kill_buf_reserve(size_t need) {
    if (need + 1 <= g_kill_cap) return;
    size_t newcap = g_kill_cap ? g_kill_cap * 2 : 64;
    while (newcap < need + 1) newcap *= 2;
    char *p = realloc(g_kill, newcap);
    if (!p) die("out of memory");
    g_kill     = p;
    g_kill_cap = newcap;
}

/* Remove [start, end) from g_text and stash the bytes in the kill
 * ring. `prepend` controls accumulation when this is a consecutive
 * kill (was_kill = true): forward kills append, backward kills
 * prepend. The first kill in a run always replaces the ring. */
static void kill_range(size_t start, size_t end, bool prepend, bool was_kill) {
    if (end > g_text_len) end = g_text_len;
    if (start >= end) return;
    size_t n = end - start;

    if (was_kill) {
        kill_buf_reserve(g_kill_len + n);
        if (prepend) {
            memmove(g_kill + n, g_kill, g_kill_len);
            memcpy(g_kill, g_text + start, n);
        } else {
            memcpy(g_kill + g_kill_len, g_text + start, n);
        }
        g_kill_len += n;
    } else {
        kill_buf_reserve(n);
        memcpy(g_kill, g_text + start, n);
        g_kill_len = n;
    }
    g_kill[g_kill_len] = '\0';
    g_last_was_kill    = true;

    buf_delete_range(start, end);
}

static void yank(void) {
    if (g_kill_len > 0) buf_insert(g_kill, g_kill_len);
}

/* --- Transpose ---
 *
 * If the cursor is in the middle of the buffer, swap the codepoint
 * before the cursor with the codepoint at the cursor and move the
 * cursor past the pair (matches GNU readline `transpose-chars`). At
 * end-of-buffer with at least two codepoints, swap the last two and
 * leave the cursor at the end. */
static void transpose_chars(void) {
    if (g_cursor == 0 || g_text_len < 2) return;

    size_t a_start, a_end, b_start, b_end;
    if (g_cursor < g_text_len) {
        a_start = prev_codepoint(g_cursor);
        a_end   = g_cursor;
        b_start = g_cursor;
        b_end   = next_codepoint(g_cursor);
    } else {
        b_start = prev_codepoint(g_cursor);
        b_end   = g_cursor;
        if (b_start == 0) return;
        a_start = prev_codepoint(b_start);
        a_end   = b_start;
    }

    size_t alen = a_end - a_start;
    size_t blen = b_end - b_start;
    /* Largest UTF-8 codepoint is 4 bytes, so 8 is comfortable. */
    char tmp[8];
    if (alen > sizeof(tmp)) return;

    memcpy(tmp, g_text + a_start, alen);
    memmove(g_text + a_start, g_text + b_start, blen);
    memcpy(g_text + a_start + blen, tmp, alen);

    if (g_cursor < g_text_len) g_cursor = b_end;
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

/* Walk g_text once to count lines and find the widest line in pixels.
 * Line 0 includes g_prompt_w as a left-side prefix (the prompt is
 * drawn before the first character of typed text). */
static void measure_text(int *out_max_line_w, int *out_n_lines) {
    int    n_lines    = 1;
    int    max_line_w = 0;
    size_t line_start = 0;
    int    line_idx   = 0;
    for (size_t i = 0; i <= g_text_len; ++i) {
        bool is_eol = (i == g_text_len) || (g_text[i] == '\n');
        if (!is_eol) continue;

        int lw = text_width_n(g_text + line_start, i - line_start);
        if (line_idx == 0) lw += g_prompt_w;
        if (lw > max_line_w) max_line_w = lw;

        if (i < g_text_len) {
            ++n_lines;
            ++line_idx;
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

    /* Draw the optional prompt at the start of row 0 in COLOR_PROMPT
     * before the buffer text. The cursor / typed text on row 0 will
     * be offset by g_prompt_w to make room. */
    if (g_prompt_len > 0) {
        draw_text_n(pixels, w, h,
                    PADDING_X, y,
                    g_prompt, g_prompt_len,
                    COLOR_PROMPT);
    }

    /* Walk the buffer and draw one segment per '\n'-delimited line.
     * On the line that contains g_cursor, measure the width of the
     * prefix from line_start to g_cursor to decide where to plant
     * the cursor bar. Line 0 starts after the prompt; later lines
     * start flush at PADDING_X. */
    size_t line_start = 0;
    int    line_idx   = 0;
    int    cursor_x   = PADDING_X + g_prompt_w;
    int    cursor_y   = y - g_ascent;

    for (size_t i = 0; i <= g_text_len; ++i) {
        bool is_eol = (i == g_text_len) || (g_text[i] == '\n');
        if (!is_eol) continue;

        int x_offset = (line_idx == 0) ? g_prompt_w : 0;

        draw_text_n(pixels, w, h,
                    PADDING_X + x_offset, y,
                    g_text + line_start, i - line_start,
                    COLOR_FG);

        if (g_cursor >= line_start && g_cursor <= i) {
            int prefix_w = text_width_n(g_text + line_start,
                                        g_cursor - line_start);
            cursor_x = PADDING_X + x_offset + prefix_w;
            cursor_y = y - g_ascent;
        }

        if (i < g_text_len) {
            ++line_idx;
            line_start = i + 1;
            y += row_h;
        }
    }

    /* Solid vertical bar cursor at g_cursor's position. */
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

/* ---------- 8. Clipboard / primary selection paste ---------- */

/* Translate one MIME advertisement into a flag bit on the offer
 * state. Anything we don't recognise is ignored — sources commonly
 * advertise rich types like text/html or image/png that we have no
 * way to render. */
static void offer_track_mime(struct paste_offer_state *st, const char *mime) {
    if (!st || !mime) return;
    if      (strcmp(mime, "text/plain;charset=utf-8") == 0) st->has_utf8        = true;
    else if (strcmp(mime, "UTF8_STRING")              == 0) st->has_utf8_string = true;
    else if (strcmp(mime, "text/plain")               == 0) st->has_plain       = true;
    else if (strcmp(mime, "STRING")                   == 0 ||
             strcmp(mime, "TEXT")                     == 0) st->has_string      = true;
}

/* Most-preferred text MIME the source advertises, or NULL if none. */
static const char *offer_best_mime(const struct paste_offer_state *st) {
    if (!st) return NULL;
    if (st->has_utf8)        return "text/plain;charset=utf-8";
    if (st->has_utf8_string) return "UTF8_STRING";
    if (st->has_plain)       return "text/plain";
    if (st->has_string)      return "STRING";
    return NULL;
}

/* --- wl_data_offer listener (clipboard MIME advertisements) --- */

static void clip_offer_offer(void *data, struct wl_data_offer *o,
                             const char *mime) {
    (void)o;
    offer_track_mime((struct paste_offer_state *)data, mime);
}

static void clip_offer_source_actions(void *data, struct wl_data_offer *o,
                                      uint32_t actions) {
    (void)data; (void)o; (void)actions;
}

static void clip_offer_action(void *data, struct wl_data_offer *o,
                              uint32_t action) {
    (void)data; (void)o; (void)action;
}

static const struct wl_data_offer_listener clip_offer_listener = {
    .offer          = clip_offer_offer,
    .source_actions = clip_offer_source_actions,
    .action         = clip_offer_action,
};

/* Replace the cached clipboard offer + state, destroying any prior. */
static void clip_offer_replace(struct wl_data_offer *new_offer) {
    if (g_clipboard_offer) {
        wl_data_offer_destroy(g_clipboard_offer);
        free(g_clipboard_offer_state);
    }
    g_clipboard_offer       = new_offer;
    g_clipboard_offer_state = new_offer ? wl_data_offer_get_user_data(new_offer)
                                        : NULL;
}

/* --- wl_data_device listener (clipboard: data_offer + selection) --- */

static void data_dev_data_offer(void *data, struct wl_data_device *d,
                                struct wl_data_offer *offer) {
    (void)data; (void)d;
    /* Allocate a fresh state and wire it to the offer. The matching
     * `selection` event will follow shortly and either promote this
     * offer to the active clipboard or replace it with NULL. */
    struct paste_offer_state *st = calloc(1, sizeof(*st));
    if (!st) { wl_data_offer_destroy(offer); return; }
    wl_data_offer_set_user_data(offer, st);
    wl_data_offer_add_listener(offer, &clip_offer_listener, st);
}

static void data_dev_selection(void *data, struct wl_data_device *d,
                               struct wl_data_offer *offer) {
    (void)data; (void)d;
    clip_offer_replace(offer);
}

/* DnD is not used by wnpt — but the listener struct demands handlers
 * for enter / leave / motion / drop, otherwise libwayland aborts on
 * the first DnD event. They no-op safely. */
static void data_dev_enter(void *data, struct wl_data_device *d,
                           uint32_t serial, struct wl_surface *surface,
                           wl_fixed_t x, wl_fixed_t y,
                           struct wl_data_offer *offer) {
    (void)data; (void)d; (void)serial; (void)surface; (void)x; (void)y;
    if (offer) wl_data_offer_destroy(offer);
}
static void data_dev_leave(void *data, struct wl_data_device *d) {
    (void)data; (void)d;
}
static void data_dev_motion(void *data, struct wl_data_device *d,
                            uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    (void)data; (void)d; (void)time; (void)x; (void)y;
}
static void data_dev_drop(void *data, struct wl_data_device *d) {
    (void)data; (void)d;
}

static const struct wl_data_device_listener data_dev_listener = {
    .data_offer = data_dev_data_offer,
    .enter      = data_dev_enter,
    .leave      = data_dev_leave,
    .motion     = data_dev_motion,
    .drop       = data_dev_drop,
    .selection  = data_dev_selection,
};

/* --- primary selection: same shape as above --- */

static void primary_offer_offer(void *data,
                                struct zwp_primary_selection_offer_v1 *o,
                                const char *mime) {
    (void)o;
    offer_track_mime((struct paste_offer_state *)data, mime);
}

static const struct zwp_primary_selection_offer_v1_listener primary_offer_listener = {
    .offer = primary_offer_offer,
};

static void primary_offer_replace(struct zwp_primary_selection_offer_v1 *new_offer) {
    if (g_primary_offer) {
        zwp_primary_selection_offer_v1_destroy(g_primary_offer);
        free(g_primary_offer_state);
    }
    g_primary_offer       = new_offer;
    g_primary_offer_state = new_offer
        ? zwp_primary_selection_offer_v1_get_user_data(new_offer)
        : NULL;
}

static void primary_dev_data_offer(void *data,
                                   struct zwp_primary_selection_device_v1 *d,
                                   struct zwp_primary_selection_offer_v1 *offer) {
    (void)data; (void)d;
    struct paste_offer_state *st = calloc(1, sizeof(*st));
    if (!st) { zwp_primary_selection_offer_v1_destroy(offer); return; }
    zwp_primary_selection_offer_v1_set_user_data(offer, st);
    zwp_primary_selection_offer_v1_add_listener(offer, &primary_offer_listener, st);
}

static void primary_dev_selection(void *data,
                                  struct zwp_primary_selection_device_v1 *d,
                                  struct zwp_primary_selection_offer_v1 *offer) {
    (void)data; (void)d;
    primary_offer_replace(offer);
}

static const struct zwp_primary_selection_device_v1_listener primary_dev_listener = {
    .data_offer = primary_dev_data_offer,
    .selection  = primary_dev_selection,
};

/* --- Reading and inserting pasted bytes --- */

/* Read everything from `fd` until EOF or a poll lull. We size the
 * timeout per chunk rather than overall: a fast source closes
 * within a few ms so the loop exits on EOF; a slow one (e.g. a
 * large remote clipboard) keeps trickling bytes and each chunk
 * resets the 2 s budget. A misbehaving source stalls at most
 * 2 s before we give up.
 *
 * Caller owns the returned buffer (free()). Returns NULL on alloc
 * failure or fatal poll/read errors. The fd is left open. */
static char *read_offer(int fd, size_t *out_len) {
    enum { POLL_TIMEOUT_MS = 2000, MAX_BYTES = 16 * 1024 * 1024 };
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    for (;;) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int n = poll(&pfd, 1, POLL_TIMEOUT_MS);
        if (n < 0) {
            if (errno == EINTR) continue;
            free(buf); return NULL;
        }
        if (n == 0) break; /* lull -> treat as done */
        if (!(pfd.revents & POLLIN)) break;

        if (len + 4096 > cap) {
            if (cap >= MAX_BYTES) break;
            size_t newcap = cap * 2;
            if (newcap > MAX_BYTES) newcap = MAX_BYTES;
            char *nb = realloc(buf, newcap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
            cap = newcap;
        }

        ssize_t r = read(fd, buf + len, cap - len);
        if (r < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            free(buf); return NULL;
        }
        if (r == 0) break; /* EOF: source closed its write end */
        len += (size_t)r;
        if (len >= MAX_BYTES) break;
    }

    *out_len = len;
    return buf;
}

/* Insert pasted bytes at the cursor with light sanitisation:
 *   - NUL bytes dropped (would break g_text's nul-termination);
 *   - CR ('\r') dropped, normalising CRLF -> LF and bare CR -> "";
 *   - everything else (incl. '\t' and '\n') is inserted verbatim.
 * Pasted newlines stay in the buffer; they do NOT commit (unlike
 * the Enter key, which is reserved for explicit submission). */
static void paste_insert(const char *src, size_t n) {
    if (!src || n == 0) return;
    char *clean = malloc(n);
    if (!clean) return;
    size_t k = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\0' || c == '\r') continue;
        clean[k++] = (char)c;
    }
    if (k > 0) buf_insert(clean, k);
    free(clean);
}

/* Common pipe + flush + slurp + insert, parameterised over the two
 * different `receive` requests. We pipe2(O_CLOEXEC) so the fd never
 * leaks anywhere; only the source app gets it (the compositor
 * forwards the write end via SCM_RIGHTS). */
static void paste_from_clipboard(void) {
    if (!g_clipboard_offer) return;
    const char *mime = offer_best_mime(g_clipboard_offer_state);
    if (!mime) return;

    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0) return;

    wl_data_offer_receive(g_clipboard_offer, mime, p[1]);
    if (wl_display_flush(g_display) < 0) { close(p[0]); close(p[1]); return; }
    close(p[1]);

    size_t len = 0;
    char *text = read_offer(p[0], &len);
    close(p[0]);
    if (text) {
        paste_insert(text, len);
        free(text);
    }
}

static void paste_from_primary(void) {
    if (!g_primary_offer) return;
    const char *mime = offer_best_mime(g_primary_offer_state);
    if (!mime) return;

    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0) return;

    zwp_primary_selection_offer_v1_receive(g_primary_offer, mime, p[1]);
    if (wl_display_flush(g_display) < 0) { close(p[0]); close(p[1]); return; }
    close(p[1]);

    size_t len = 0;
    char *text = read_offer(p[0], &len);
    close(p[0]);
    if (text) {
        paste_insert(text, len);
        free(text);
    }
}

/* ---------- 9. Wayland listeners ---------- */

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

    /* Capture and reset the kill flag at the start of each command;
     * each kill_*() helper re-asserts it, so two consecutive kill
     * commands (e.g. Ctrl+K Ctrl+K) accumulate their text into a
     * single kill-ring entry. */
    bool was_kill   = g_last_was_kill;
    g_last_was_kill = false;

    /* Wayland reports evdev keycodes; xkb expects evdev + 8. */
    xkb_keycode_t keycode = key + 8;

    /* Honour the user's current layout (this is a text-input prompt,
     * so Cyrillic / Greek / Dvorak should produce native glyphs). */
    xkb_keysym_t sym       = xkb_state_key_get_one_sym(g_xkb_state, keycode);
    xkb_keysym_t sym_lower = xkb_keysym_to_lower(sym);

    bool ctrl  = xkb_state_mod_name_is_active(
        g_xkb_state, XKB_MOD_NAME_CTRL,  XKB_STATE_MODS_EFFECTIVE) > 0;
    bool shift = xkb_state_mod_name_is_active(
        g_xkb_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0;
    bool alt   = xkb_state_mod_name_is_active(
        g_xkb_state, XKB_MOD_NAME_ALT,   XKB_STATE_MODS_EFFECTIVE) > 0;
    bool super = xkb_state_mod_name_is_active(
        g_xkb_state, XKB_MOD_NAME_LOGO,  XKB_STATE_MODS_EFFECTIVE) > 0;

    /* ---------- Submission / cancel (highest priority) ---------- */

    if (sym == XKB_KEY_Escape ||
        (ctrl && sym_lower == XKB_KEY_g)) {
        g_committed = 0;
        g_running   = 0;
        return;
    }

    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        if (shift) {
            buf_insert("\n", 1);
            schedule_redraw();
        } else {
            g_committed = 1;
            g_running   = 0;
        }
        return;
    }

    /* ---------- Cursor movement (named keys) ---------- */

    if (sym == XKB_KEY_Left) {
        g_cursor = ctrl ? word_back(g_cursor) : prev_codepoint(g_cursor);
        schedule_redraw();
        return;
    }
    if (sym == XKB_KEY_Right) {
        g_cursor = ctrl ? word_forward(g_cursor) : next_codepoint(g_cursor);
        schedule_redraw();
        return;
    }
    if (sym == XKB_KEY_Up)   { cursor_up();   schedule_redraw(); return; }
    if (sym == XKB_KEY_Down) { cursor_down(); schedule_redraw(); return; }
    if (sym == XKB_KEY_Home) { g_cursor = line_start_at(g_cursor);
                               schedule_redraw(); return; }
    if (sym == XKB_KEY_End)  { g_cursor = line_end_at(g_cursor);
                               schedule_redraw(); return; }

    /* Shift+Insert pastes the primary selection (mouse-highlight
     * buffer). Plain Insert and other modifier combos are ignored
     * — we don't model overwrite mode, and Ctrl+Insert (a copy
     * alias on some desktops) has nothing to copy from wnpt. */
    if (sym == XKB_KEY_Insert) {
        if (shift && !ctrl && !alt && !super) {
            paste_from_primary();
            schedule_redraw();
        }
        return;
    }

    /* ---------- Editing (named keys) ---------- */

    if (sym == XKB_KEY_BackSpace) {
        if (ctrl) {
            kill_range(word_back(g_cursor), g_cursor,
                       /*prepend=*/true, was_kill);
        } else {
            buf_delete_range(prev_codepoint(g_cursor), g_cursor);
        }
        schedule_redraw();
        return;
    }
    if (sym == XKB_KEY_Delete) {
        buf_delete_range(g_cursor, next_codepoint(g_cursor));
        schedule_redraw();
        return;
    }

    /* ---------- Ctrl+letter chords (no Alt / Super) ---------- */

    if (ctrl && !alt && !super) {
        switch (sym_lower) {
        case XKB_KEY_b:
            g_cursor = prev_codepoint(g_cursor);
            schedule_redraw(); return;
        case XKB_KEY_f:
            g_cursor = next_codepoint(g_cursor);
            schedule_redraw(); return;
        case XKB_KEY_a:
            g_cursor = line_start_at(g_cursor);
            schedule_redraw(); return;
        case XKB_KEY_e:
            g_cursor = line_end_at(g_cursor);
            schedule_redraw(); return;
        case XKB_KEY_h: /* alias for Backspace */
            buf_delete_range(prev_codepoint(g_cursor), g_cursor);
            schedule_redraw(); return;
        case XKB_KEY_d: /* alias for Delete */
            buf_delete_range(g_cursor, next_codepoint(g_cursor));
            schedule_redraw(); return;
        case XKB_KEY_w:
            kill_range(word_back(g_cursor), g_cursor,
                       /*prepend=*/true, was_kill);
            schedule_redraw(); return;
        case XKB_KEY_k: {
            /* Kill to end of line. If already at EOL and a '\n'
             * follows, eat that too (matches readline semantics). */
            size_t e = line_end_at(g_cursor);
            if (e == g_cursor && e < g_text_len) e = g_cursor + 1;
            kill_range(g_cursor, e, /*prepend=*/false, was_kill);
            schedule_redraw(); return;
        }
        case XKB_KEY_u:
            kill_range(line_start_at(g_cursor), g_cursor,
                       /*prepend=*/true, was_kill);
            schedule_redraw(); return;
        case XKB_KEY_y:
            yank();
            schedule_redraw(); return;
        case XKB_KEY_t:
            transpose_chars();
            schedule_redraw(); return;
        case XKB_KEY_v:
            /* Ctrl+V pastes the Wayland clipboard selection
             * (the wl_data_device one — what other apps' Ctrl+C
             * writes into). */
            paste_from_clipboard();
            schedule_redraw(); return;
        }
        /* Any other Ctrl+key is silently ignored; we don't want
         * Ctrl combos to leak into the text buffer. */
        return;
    }

    /* ---------- Alt+letter chords (no Ctrl / Super) ---------- */

    if (alt && !ctrl && !super) {
        switch (sym_lower) {
        case XKB_KEY_b:
            g_cursor = word_back(g_cursor);
            schedule_redraw(); return;
        case XKB_KEY_f:
            g_cursor = word_forward(g_cursor);
            schedule_redraw(); return;
        case XKB_KEY_d:
            kill_range(g_cursor, word_forward(g_cursor),
                       /*prepend=*/false, was_kill);
            schedule_redraw(); return;
        case XKB_KEY_BackSpace:
            kill_range(word_back(g_cursor), g_cursor,
                       /*prepend=*/true, was_kill);
            schedule_redraw(); return;
        }
        return;
    }

    /* Drop everything else with a non-shift modifier held; we don't
     * want stray modifier combos to insert text. */
    if (ctrl || alt || super) return;

    /* ---------- Plain text insertion ---------- */

    char utf8[16];
    int n = xkb_state_key_get_utf8(g_xkb_state, keycode,
                                   utf8, sizeof(utf8));
    if (n <= 0) return;

    /* Filter ASCII control characters (other than '\t'). Newlines are
     * routed through the Return path above. */
    if (n == 1) {
        unsigned char c = (unsigned char)utf8[0];
        if (c < 0x20 && c != '\t') return;
        if (c == 0x7F) return;
    }

    buf_insert(utf8, (size_t)n);
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
    } else if (strcmp(iface, wl_data_device_manager_interface.name) == 0) {
        uint32_t v = version > 3 ? 3 : version;
        g_data_dev_mgr = wl_registry_bind(
            r, name, &wl_data_device_manager_interface, v);
    } else if (strcmp(iface,
               zwp_primary_selection_device_manager_v1_interface.name) == 0) {
        g_primary_mgr = wl_registry_bind(
            r, name, &zwp_primary_selection_device_manager_v1_interface, 1);
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

/* ---------- 10. main ---------- */

static void usage(void) {
    fputs(
        "usage: wnpt [-f FONT] [-p PROMPT]\n"
        "  Reads typed text from a Wayland overlay window with readline-\n"
        "  style line editing. On Enter, prints the buffered text to\n"
        "  stdout and exits 0; on Esc or Ctrl-G, exits 1 without\n"
        "  printing anything. Shift+Enter inserts a newline.\n"
        "\n"
        "  Movement : Ctrl+B/F (char), Alt+B/F or Ctrl+Left/Right (word),\n"
        "             Ctrl+A/E or Home/End (line), Up/Down (multi-line).\n"
        "  Editing  : Backspace / Ctrl+H, Delete / Ctrl+D,\n"
        "             Ctrl+W / Ctrl+Backspace / Alt+Backspace (kill word back),\n"
        "             Alt+D (kill word forward), Ctrl+K (kill to EOL),\n"
        "             Ctrl+U (kill to BOL), Ctrl+Y (yank), Ctrl+T (transpose).\n"
        "  Pasting  : Ctrl+V (clipboard), Shift+Insert (primary selection).\n"
        "\n"
        "  -f FONT     fontconfig pattern (env: WNPT_FONT, then WLNCH_FONT)\n"
        "  -p PROMPT   single-line text to display before the input area;\n"
        "              shown in COLOR_PROMPT, never written to stdout\n",
        stderr);
}

int main(int argc, char **argv) {
    int opt;
    while ((opt = getopt(argc, argv, "f:p:h")) != -1) {
        switch (opt) {
        case 'f': g_font_pattern = optarg; break;
        case 'p': g_prompt       = optarg; break;
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

    /* Reject multi-line prompts: the rendering model assumes the
     * prompt is a single-line prefix to row 0. Embedded newlines
     * would either get rendered as zero-advance glyphs (ugly) or
     * require a more involved layout pass; bail clearly instead. */
    if (g_prompt) {
        if (strchr(g_prompt, '\n') || strchr(g_prompt, '\r'))
            die("-p PROMPT must not contain newline characters");
        g_prompt_len = strlen(g_prompt);
    }

    font_init(g_font_pattern);

    /* Cache the prompt's pixel width once font metrics are available. */
    if (g_prompt_len > 0)
        g_prompt_w = text_width_n(g_prompt, g_prompt_len);

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

    /* Bind the per-seat clipboard / primary-selection devices.
     * Either manager being absent is non-fatal: the matching paste
     * binding (Ctrl+V / Shift+Insert) just becomes a no-op. */
    if (g_data_dev_mgr && g_seat) {
        g_data_device = wl_data_device_manager_get_data_device(
            g_data_dev_mgr, g_seat);
        wl_data_device_add_listener(g_data_device, &data_dev_listener, NULL);
    }
    if (g_primary_mgr && g_seat) {
        g_primary_device = zwp_primary_selection_device_manager_v1_get_device(
            g_primary_mgr, g_seat);
        zwp_primary_selection_device_v1_add_listener(
            g_primary_device, &primary_dev_listener, NULL);
    }

    /* Third roundtrip so the initial `data_offer` + `selection`
     * events for whatever is already on the clipboards arrive
     * before we enter the dispatch loop. Without this, an immediate
     * Ctrl+V right after launch might find g_clipboard_offer still
     * NULL even though the desktop has a perfectly good selection. */
    if (g_data_device || g_primary_device)
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

    /* Drop any cached selection offers and the per-seat devices
     * before tearing down the seat / managers / compositor. */
    clip_offer_replace(NULL);
    primary_offer_replace(NULL);
    if (g_data_device)   wl_data_device_destroy(g_data_device);
    if (g_primary_device) zwp_primary_selection_device_v1_destroy(g_primary_device);
    if (g_data_dev_mgr)  wl_data_device_manager_destroy(g_data_dev_mgr);
    if (g_primary_mgr)   zwp_primary_selection_device_manager_v1_destroy(g_primary_mgr);

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
    free(g_kill);

    /* Exit code mirrors commit/cancel: 0 if the user pressed Enter,
     * 1 otherwise (Esc / Ctrl-G / surface-closed). */
    return g_committed ? 0 : 1;
}
