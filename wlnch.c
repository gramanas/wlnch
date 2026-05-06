/*
 * wlnch - a tiny Wayland command launcher.
 *
 * Single translation unit. Layout:
 *
 *   1. Includes (compile-time configuration lives in config.h)
 *   2. Globals (config, wayland, xkb, font, window state)
 *   3. Utility helpers
 *   4. Config parser
 *   5. Spawning
 *   6. Font / text rendering
 *   7. SHM buffer allocation
 *   8. Drawing
 *   9. Wayland listeners (registry, seat, keyboard, layer surface)
 *  10. main()
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
#include <sys/wait.h>
#include <unistd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <fontconfig/fontconfig.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include "config.h"

/* ---------- 2. Globals ---------- */

struct entry {
    uint32_t      codepoint;       /* unicode value of the key character */
    xkb_keysym_t  keysym;          /* matching xkb keysym */
    bool          sticky;          /* `KEY&:NAME:CMD` => keep running after spawn */
    bool          separator_after; /* a `---` line followed this entry */
    bool          has_key_color;   /* `KEY#RRGGBB:` overrides the key letter color */
    uint32_t      key_color;       /* ARGB, alpha forced to 0xFF; valid iff has_key_color */
    char         *name;
    char         *command;
};

static struct entry                 * g_entries;
static size_t                         g_n_entries;

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
static int                            g_buffer_attached = 0;
static int                            g_win_w     = 0;
static int                            g_win_h     = 0;

static const char *g_font_pattern = NULL; /* fontconfig pattern */

/* ---------- 3. Utilities ---------- */

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "wlnch: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static char *xstrdup(const char *s) {
    char *p = strdup(s);
    if (!p) die("out of memory");
    return p;
}

/* Decode a single UTF-8 sequence at `s` into a codepoint, return number of
 * bytes consumed, or 0 on error. */
static int utf8_decode(const char *s, uint32_t *out) {
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] < 0x80) { *out = u[0]; return 1; }
    if ((u[0] & 0xE0) == 0xC0 && (u[1] & 0xC0) == 0x80) {
        *out = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F);
        return 2;
    }
    if ((u[0] & 0xF0) == 0xE0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80) {
        *out = ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F);
        return 3;
    }
    if ((u[0] & 0xF8) == 0xF0 && (u[1] & 0xC0) == 0x80 &&
        (u[2] & 0xC0) == 0x80 && (u[3] & 0xC0) == 0x80) {
        *out = ((u[0] & 0x07) << 18) | ((u[1] & 0x3F) << 12) |
               ((u[2] & 0x3F) << 6)  |  (u[3] & 0x3F);
        return 4;
    }
    return 0;
}

/* ---------- 4. Config parser ---------- */

static void rstrip(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' ||
                     s[n-1] == ' '  || s[n-1] == '\t')) {
        s[--n] = '\0';
    }
}

/* Parse the config from `f`. `source` is a human-readable label used in
 * error messages (a file path, or "<stdin>"). `f` is not closed here. */
static void parse_config(FILE *f, const char *source) {
    char   line[4096];
    int    lineno = 0;
    size_t cap = 0;

    /* A leading `#!/usr/local/bin/wlnch` shebang is naturally ignored
     * by the comment-skip logic below, so the config file can be made
     * executable and run directly with no extra parser handling. */

    while (fgets(line, sizeof(line), f)) {
        ++lineno;
        rstrip(line);

        char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '\0' || *p == '#') continue;

        /* A line consisting solely of `---` (after stripping whitespace)
         * marks the previous entry as having a visual separator after it.
         * Lone `---` before any entry is silently ignored; consecutive
         * `---` lines collapse to a single separator. */
        if (strcmp(p, "---") == 0) {
            if (g_n_entries > 0)
                g_entries[g_n_entries - 1].separator_after = true;
            continue;
        }

        /* Field 1: KEY [&|#RRGGBB] then ':'.  Between the key character
         * and the colon, an optional modifier may appear (mutually
         * exclusive):
         *   '&'        -> sticky entry: command runs but wlnch keeps
         *                 running so the key can be pressed again.
         *   '#RRGGBB'  -> override the key letter color (6 hex digits,
         *                 alpha is always 0xFF). Sticky entries always
         *                 use COLOR_KEY_STICKY, so this is rejected
         *                 alongside '&'. */
        uint32_t cp;
        int klen = utf8_decode(p, &cp);
        if (klen == 0)
            die("config %s:%d: invalid UTF-8 in key", source, lineno);

        bool     sticky        = false;
        bool     has_key_color = false;
        uint32_t key_color     = 0;
        int      off           = klen;

        if (p[off] == '&') {
            sticky = true;
            ++off;
        } else if (p[off] == '#') {
            ++off;
            uint32_t rgb = 0;
            for (int i = 0; i < 6; ++i) {
                char c = p[off + i];
                int  d;
                if      (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else die("config %s:%d: expected 6 hex digits after '#'",
                         source, lineno);
                rgb = (rgb << 4) | (uint32_t)d;
            }
            off += 6;
            key_color     = 0xFF000000u | rgb;
            has_key_color = true;
        }

        if (p[off] != ':')
            die("config %s:%d: expected ':' after key "
                "(with optional '&' or '#RRGGBB', not both)",
                source, lineno);

        char *name_start = p + off + 1;
        char *colon2 = strchr(name_start, ':');
        if (!colon2)
            die("config %s:%d: expected ':' between name and command",
                source, lineno);

        *colon2 = '\0';
        char *name = name_start;
        char *cmd  = colon2 + 1;

        if (*name == '\0')
            die("config %s:%d: empty name", source, lineno);
        if (*cmd == '\0')
            die("config %s:%d: empty command", source, lineno);

        if (g_n_entries == cap) {
            cap = cap ? cap * 2 : 16;
            g_entries = realloc(g_entries, cap * sizeof(*g_entries));
            if (!g_entries) die("out of memory");
        }
        g_entries[g_n_entries].codepoint       = cp;
        g_entries[g_n_entries].keysym          = xkb_utf32_to_keysym(cp);
        g_entries[g_n_entries].sticky          = sticky;
        g_entries[g_n_entries].separator_after = false;
        g_entries[g_n_entries].has_key_color   = has_key_color;
        g_entries[g_n_entries].key_color       = key_color;
        g_entries[g_n_entries].name            = xstrdup(name);
        g_entries[g_n_entries].command         = xstrdup(cmd);
        ++g_n_entries;
    }

    if (g_n_entries == 0)
        die("config %s contains no entries", source);
}

/* ---------- 5. Spawning ---------- */

/* Fork twice so the launched process is reparented to PID 1, then exec the
 * command via /bin/sh -c so shell pipelines / quoting work as expected. */
static void spawn_command(const char *cmd) {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "wlnch: fork failed: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        if (setsid() < 0) _exit(127);
        pid_t pid2 = fork();
        if (pid2 < 0) _exit(127);
        if (pid2 == 0) {
            /* Detach from the launcher's stdio so closing it doesn't
             * SIGPIPE the child. */
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                if (devnull > STDERR_FILENO) close(devnull);
            }
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    /* Reap the immediate child to avoid a zombie. */
    int status;
    waitpid(pid, &status, 0);
}

/* ---------- 6. Font / text rendering ---------- */

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
    /* FC_PIXEL_SIZE may be set if the user specified it in the pattern. */
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

/* Compute pixel width of a UTF-8 string using only x-advance. */
static int text_width(const char *s) {
    int x = 0;
    uint32_t cp;
    while (*s) {
        int n = utf8_decode(s, &cp);
        if (n == 0) { ++s; continue; }
        if (FT_Load_Char(g_ft_face, cp, FT_LOAD_DEFAULT) == 0)
            x += g_ft_face->glyph->advance.x >> 6;
        s += n;
    }
    return x;
}

/* Blend a single rendered glyph bitmap into the ARGB pixel buffer. */
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

/* Render a UTF-8 string starting at (pen_x, baseline_y). Returns advance. */
static int draw_text(uint32_t *pixels, int w, int h,
                     int pen_x, int baseline_y,
                     const char *s, uint32_t color) {
    int x = pen_x;
    uint32_t cp;
    while (*s) {
        int n = utf8_decode(s, &cp);
        if (n == 0) { ++s; continue; }
        if (FT_Load_Char(g_ft_face, cp, FT_LOAD_RENDER) == 0) {
            blit_glyph(pixels, w, h, x, baseline_y, color);
            x += g_ft_face->glyph->advance.x >> 6;
        }
        s += n;
    }
    return x - pen_x;
}

/* ---------- 7. SHM buffer ---------- */

static int create_shm_fd(size_t size) {
    int fd = memfd_create("wlnch-shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static struct wl_buffer *create_shm_buffer(int w, int h, uint32_t **pixels_out) {
    int stride = w * 4;
    size_t size = (size_t)stride * (size_t)h;

    int fd = create_shm_fd(size);
    if (fd < 0) die("memfd_create failed: %s", strerror(errno));

    void *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) die("mmap failed: %s", strerror(errno));

    struct wl_shm_pool *pool = wl_shm_create_pool(g_shm, fd, (int32_t)size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(
        pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    *pixels_out = (uint32_t *)mem;
    return buf;
}

/* ---------- 8. Drawing ---------- */

/* Encode a codepoint as a nul-terminated UTF-8 string. Buffer must be >=5
 * bytes. Returns the number of bytes written (excluding the terminator). */
static int utf8_encode(uint32_t cp, char *out) {
    int n = 0;
    if (cp < 0x80) {
        out[n++] = (char)cp;
    } else if (cp < 0x800) {
        out[n++] = (char)(0xC0 | (cp >> 6));
        out[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out[n++] = (char)(0xE0 | (cp >> 12));
        out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        out[n++] = (char)(0xF0 | (cp >> 18));
        out[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[n++] = (char)(0x80 | ((cp >>  6) & 0x3F));
        out[n++] = (char)(0x80 | (cp & 0x3F));
    }
    out[n] = '\0';
    return n;
}

/* Build the bracketed-key label for an entry: "[k]". Stickiness is
 * conveyed by color in render_frame, not by extra glyphs, so all key
 * cells render to the same width. */
static void key_label_str(const struct entry *e, char *out) {
    int n = 0;
    out[n++] = '[';
    n += utf8_encode(e->codepoint, out + n);
    out[n++] = ']';
    out[n]   = '\0';
}

/* Compute window dimensions based on the longest row "[k]   name". */
static void compute_window_size(void) {
    int key_cell_w = 0;
    for (size_t i = 0; i < g_n_entries; ++i) {
        char buf[16];
        key_label_str(&g_entries[i], buf);
        int kw = text_width(buf);
        if (kw > key_cell_w) key_cell_w = kw;
    }

    int max_name_w = 0;
    for (size_t i = 0; i < g_n_entries; ++i) {
        int nw = text_width(g_entries[i].name);
        if (nw > max_name_w) max_name_w = nw;
    }

    int row_w = key_cell_w + KEY_GAP + max_name_w;
    int row_h = g_line_height + ROW_GAP;

    /* Separators only contribute height when there's an entry after them;
     * a trailing `---` is treated as a no-op so the window doesn't grow. */
    int n_separators = 0;
    for (size_t i = 0; i + 1 < g_n_entries; ++i)
        if (g_entries[i].separator_after) ++n_separators;

    g_win_w = PADDING_X * 2 + row_w;
    g_win_h = PADDING_Y * 2 + (int)g_n_entries * row_h - ROW_GAP
              + n_separators * row_h;

    /* Round up to multiples of 2 to avoid stride oddities. */
    if (g_win_w & 1) g_win_w++;
    if (g_win_h & 1) g_win_h++;
}

static void fill_rect(uint32_t *pixels, int w, int h, uint32_t color) {
    int n = w * h;
    for (int i = 0; i < n; ++i) pixels[i] = color;
}

/* Punch transparent pixels into the four window corners so the surface
 * appears rounded. Each corner is masked against a quarter circle of
 * radius `radius`; pixels strictly outside the circle are zeroed,
 * and the 1-pixel boundary band is alpha-faded for cheap AA. The
 * compositor then composites whatever is behind those transparent
 * pixels, giving the illusion of a non-rectangular window.
 *
 * Only the alpha channel is scaled for AA pixels. The current bg uses
 * straight alpha (matching how text glyphs are blended elsewhere in
 * this file), so we don't need to also scale RGB. */
static void apply_rounded_corners(uint32_t *pixels, int w, int h, int radius) {
    if (radius <= 0) return;

    int r = radius;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    for (int j = 0; j < r; ++j) {
        for (int i = 0; i < r; ++i) {
            /* Distance from the pixel center to the corner's arc center.
             * The arc center sits `r` pixels in from each edge, so for
             * the top-left corner the arc center is at (r, r) and the
             * pixel at index (i, j) has its center at (i+0.5, j+0.5). */
            float dx = (float)r - 0.5f - (float)i;
            float dy = (float)r - 0.5f - (float)j;
            float dist = sqrtf(dx * dx + dy * dy);

            float coverage;
            if (dist <= (float)r - 0.5f)      coverage = 1.0f;
            else if (dist >= (float)r + 0.5f) coverage = 0.0f;
            else                              coverage = (float)r + 0.5f - dist;

            if (coverage >= 1.0f) continue; /* fully inside the disc */

            /* Mirror to all four corners. */
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

static void render_frame(uint32_t *pixels, int w, int h) {
    fill_rect(pixels, w, h, COLOR_BG);

    /* Determine the widest "[k]" so names line up. */
    int key_cell_w = 0;
    for (size_t i = 0; i < g_n_entries; ++i) {
        char buf[16];
        key_label_str(&g_entries[i], buf);
        int kw = text_width(buf);
        if (kw > key_cell_w) key_cell_w = kw;
    }

    int row_h = g_line_height + ROW_GAP;
    int y = PADDING_Y + g_ascent;

    for (size_t i = 0; i < g_n_entries; ++i) {
        int x = PADDING_X;

        /* Bracketed key label. The brackets are always in the separator
         * color; the key letter is accent-colored, with sticky entries
         * using a distinct reddish accent. */
        char ch_buf[8];
        utf8_encode(g_entries[i].codepoint, ch_buf);

        /* Sticky entries always use COLOR_KEY_STICKY so the visual cue
         * for stickiness can't be overridden. Otherwise honour an
         * optional per-entry `#RRGGBB`, falling back to COLOR_KEY. */
        uint32_t key_color = g_entries[i].sticky
            ? COLOR_KEY_STICKY
            : (g_entries[i].has_key_color
                ? g_entries[i].key_color
                : COLOR_KEY);

        x += draw_text(pixels, w, h, x, y, "[",    COLOR_SEP);
        x += draw_text(pixels, w, h, x, y, ch_buf, key_color);
        x += draw_text(pixels, w, h, x, y, "]",    COLOR_SEP);

        /* Pad so names line up. */
        int row_x = PADDING_X + key_cell_w + KEY_GAP;
        if (x < row_x) x = row_x;

        draw_text(pixels, w, h, x, y, g_entries[i].name, COLOR_FG);

        y += row_h;

        /* Insert one extra blank row after entries flagged with `---`,
         * but only between entries — a trailing flag adds no padding. */
        if (g_entries[i].separator_after && i + 1 < g_n_entries)
            y += row_h;
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

    /* The compositor takes ownership of the buffer for the duration of its
     * use; we destroy it on release. We can also just listen for the
     * release event, but for a one-shot launcher this is fine: the buffer
     * will be released when we exit. */
    g_buffer_attached = 1;
    (void)buf;
}

/* ---------- 9. Wayland listeners ---------- */

/* --- layer surface --- */

static void layer_surface_configure(void *data,
                                    struct zwlr_layer_surface_v1 *s,
                                    uint32_t serial,
                                    uint32_t width, uint32_t height) {
    (void)data; (void)width; (void)height;
    zwlr_layer_surface_v1_ack_configure(s, serial);
    if (!g_buffer_attached) {
        draw_and_attach();
    }
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

    /* Resolve the keysym against layout 0 (the primary layout) instead of
     * the currently active group, so Latin-letter bindings still trigger
     * when the user has switched to Cyrillic / Greek / Dvorak / etc.
     * Shift state is still honored via the level lookup. */
    xkb_layout_index_t layout = 0;
    xkb_level_index_t  level  = xkb_state_key_get_level(
        g_xkb_state, keycode, layout);
    const xkb_keysym_t *syms = NULL;
    int n_syms = xkb_keymap_key_get_syms_by_level(
        g_xkb_keymap, keycode, layout, level, &syms);
    xkb_keysym_t sym = (n_syms > 0) ? syms[0] : XKB_KEY_NoSymbol;

    /* Always exit on Escape. */
    if (sym == XKB_KEY_Escape || sym == XKB_KEY_q) {
        g_running = 0;
        return;
    }

    int ctrl_active = xkb_state_mod_name_is_active(
        g_xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0;
    int alt_active = xkb_state_mod_name_is_active(
        g_xkb_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0;
    int super_active = xkb_state_mod_name_is_active(
        g_xkb_state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0;

    /* Always exit on Ctrl-G (or Ctrl-Shift-G). */
    if (ctrl_active && (sym == XKB_KEY_g || sym == XKB_KEY_G)) {
        g_running = 0;
        return;
    }

    /* Don't dispatch entries when ctrl/alt/super is held; those combos are
     * not part of the configured keybinds and would be confusing. */
    if (ctrl_active || alt_active || super_active) return;

    for (size_t i = 0; i < g_n_entries; ++i) {
        if (g_entries[i].keysym == sym) {
            spawn_command(g_entries[i].command);
            if (!g_entries[i].sticky) g_running = 0;
            return;
        }
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

/* ---------- 10. main ---------- */

static void usage(void) {
    fputs(
        "usage: wlnch [-f FONT] [CONFIG]\n"
        "  CONFIG    path to config file; if omitted, read from stdin\n"
        "  -f FONT   fontconfig pattern (default: monospace, env: WLNCH_FONT)\n"
        "\n"
        "  A wlnch config can carry a `#!/usr/local/bin/wlnch` shebang and\n"
        "  be made executable so that running it directly opens the launcher.\n",
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

    if (!g_font_pattern) {
        const char *env = getenv("WLNCH_FONT");
        g_font_pattern = (env && *env) ? env : DEFAULT_FONT;
    }

    if (argc - optind > 1) {
        usage();
        return 2;
    }

    const char *config_path = (optind < argc) ? argv[optind] : NULL;

    FILE       *cfg_fp     = NULL;
    const char *cfg_source = NULL;
    if (config_path) {
        cfg_fp = fopen(config_path, "r");
        if (!cfg_fp)
            die("cannot open config %s: %s", config_path, strerror(errno));
        cfg_source = config_path;
    } else {
        cfg_fp     = stdin;
        cfg_source = "<stdin>";
    }

    parse_config(cfg_fp, cfg_source);
    if (cfg_fp != stdin) fclose(cfg_fp);

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
            "       (this includes GNOME/Mutter; wlnch needs Sway, Hyprland, "
            "KDE, etc.)");

    /* Second roundtrip so seat capability events arrive and we get a
     * keyboard before going further. */
    wl_display_roundtrip(g_display);

    g_surface = wl_compositor_create_surface(g_compositor);
    g_layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        g_layer_shell, g_surface, NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "wlnch");

    zwlr_layer_surface_v1_add_listener(
        g_layer_surface, &layer_surface_listener, NULL);
    zwlr_layer_surface_v1_set_size(
        g_layer_surface, (uint32_t)g_win_w, (uint32_t)g_win_h);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        g_layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    /* No anchors -> centered. */

    wl_surface_commit(g_surface);

    /* Main loop. */
    while (g_running && wl_display_dispatch(g_display) != -1) {
        /* nothing */
    }

    /* Teardown. */
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

    for (size_t i = 0; i < g_n_entries; ++i) {
        free(g_entries[i].name);
        free(g_entries[i].command);
    }
    free(g_entries);

    return 0;
}
