/*
 * config.h - wlnch compile-time configuration
 *
 * All user-tweakable visual defaults live in this file. Recompile after
 * editing (`make`). The runtime config file
 * (KEY[&|#RRGGBB]:NAME:COMMAND) decides *what* to display; this header
 * decides *how* it looks.
 *
 * Color encoding: every color is a 32-bit literal in 0xAARRGGBB form.
 * Compositing uses straight (non-premultiplied) alpha. The Wayland
 * surface format is WL_SHM_FORMAT_ARGB8888, which is little-endian
 * byte order; on a little-endian host a uint32_t literal 0xAARRGGBB
 * lays out as B,G,R,A in memory, which is what the compositor wants.
 */

#ifndef WLNCH_CONFIG_H
#define WLNCH_CONFIG_H

/* ---------- Font ---------- */

/* Fontconfig pattern used when neither the -f FONT command-line flag
 * nor the $WLNCH_FONT environment variable is set. Any valid
 * fontconfig pattern works, e.g. "monospace:size=18",
 * "Iosevka:size=24:weight=bold", "Sans 14". The :size= suffix sets
 * the pixel size; without it, DEFAULT_FONT_PIXEL is used as a
 * fallback.
 * Default: "liberation mono:size=32". */
#define DEFAULT_FONT       "liberation mono:size=32"

/* Fallback pixel size handed to FreeType when the matched fontconfig
 * pattern does not carry an explicit FC_PIXEL_SIZE. Has no effect for
 * patterns that already include ":size=..." (which DEFAULT_FONT does).
 * Default: 32. */
#define DEFAULT_FONT_PIXEL 32

/* ---------- Colors (0xAARRGGBB) ---------- */

/* Window background color. The alpha channel controls opacity over
 * whatever the compositor draws underneath (wallpaper / blur).
 * 0xFF alpha is fully opaque, 0x00 is fully transparent; the default
 * is roughly 94% opaque so the launcher reads as solid but lets a
 * hint of the background through on blur-capable compositors.
 * Default: 0xF0242424 (very dark grey, ~94% opaque). */
#define COLOR_BG         0xF0242424U

/* Foreground color used for the entry NAME (the text after the
 * bracketed key label). Should normally be fully opaque (alpha 0xFF).
 * Default: 0xFFF6F3E8 (off-white). */
#define COLOR_FG         0xFFF6F3E8U

/* Default color of the key letter inside `[k]` for non-sticky
 * entries. A per-entry `KEY#RRGGBB:NAME:COMMAND` directive in the
 * config file overrides this on a row-by-row basis; sticky entries
 * always use COLOR_KEY_STICKY instead and ignore both this and any
 * per-entry override.
 * Default: 0xFF8AB4F8 (light blue accent). */
#define COLOR_KEY        0xFF8AB4F8U

/* Color of the key letter for *sticky* entries (those with `&`
 * between the key and the first `:`). This always wins over both
 * COLOR_KEY and any per-entry `#RRGGBB`, so the sticky visual cue
 * cannot be hidden by a config typo.
 * Default: 0xFFE06B6B (reddish accent). */
#define COLOR_KEY_STICKY 0xFFE06B6BU

/* Color of the brackets around the key letter (`[` and `]`).
 * Drawing them in pure black against the dark background makes the
 * brackets nearly invisible, which keeps focus on the key letter;
 * lighten this for a more pronounced bracket frame.
 * Default: 0xFF000000 (opaque black). */
#define COLOR_SEP        0xF0242424U

/* ---------- Layout (pixels) ---------- */

/* Horizontal padding between the window edge and the row content,
 * applied symmetrically on the left and right side.
 * Default: 24. */
#define PADDING_X 24

/* Vertical padding between the window edge and the first / last
 * row, applied symmetrically on the top and bottom.
 * Default: 18. */
#define PADDING_Y 18

/* Extra vertical gap *between* consecutive rows, added on top of
 * the font's own line height. Larger values spread the list out;
 * 0 packs rows as tightly as the font allows. Also used as the
 * height of the blank gap inserted by `---` separators in the
 * config file (one extra row_h = line_height + ROW_GAP).
 * Default: 6. */
#define ROW_GAP    6

/* Horizontal gap between the closing `]` of the key label and the
 * start of the entry name. All names align at the same x position
 * (computed from the widest `[k]` cell), so this is the gutter
 * width between the key column and the name column.
 * Default: 18. */
#define KEY_GAP   18

/* Radius (in pixels) of the four window corners. The corner pixels
 * outside the rounded mask are written as fully transparent so the
 * compositor draws what's behind them. Setting this to 0 disables
 * the corner mask entirely (sharp rectangular window). The value is
 * silently clamped to half the smaller window dimension, so a huge
 * radius just gives a fully circular / stadium-shaped window.
 * Default: 12. */
#define CORNER_RADIUS 18

/* ---------- wnpt (note prompt) ---------- */

/* Color of the cursor bar drawn at the end of the typed text in
 * wnpt. By default it tracks COLOR_FG so the cursor matches the
 * text color; override for a more / less prominent caret.
 * Default: COLOR_FG. */
#define CURSOR_COLOR COLOR_FG

/* Color used to render the optional prompt (`wnpt -p PROMPT`)
 * shown at the start of the first row before the user's input.
 * Tracking COLOR_KEY by default keeps the visual signature
 * consistent with wlnch's accent.
 * Default: COLOR_KEY. */
#define COLOR_PROMPT COLOR_KEY

/* Width (in pixels) of the cursor bar.
 * Default: 2. */
#define CURSOR_WIDTH 2

/* Minimum width (in pixels) of the wnpt window. Used as the floor
 * even when the typed text is short or the buffer is empty, so the
 * prompt always presents a usable text area. Pure-text mode has
 * no key-label column to anchor a sensible width otherwise.
 * Default: 480. */
#define WNPT_MIN_WIDTH 480

/* Maximum width (in pixels) of the wnpt window. Lines longer than
 * this clip at the right edge instead of growing the window past
 * the screen. There is no scrolling or wrapping; a long line just
 * runs off the end of the visible area until you delete or newline.
 * Default: 1200. */
#define WNPT_MAX_WIDTH 1200

/* ---------- wout (stdin viewer) ---------- */

/* Minimum width (in pixels) of the wout window. Floors the
 * window even when the piped input is short, so the result still
 * looks like a deliberate dialog rather than a pixel-thin sliver.
 * Default: 480. */
#define WOUT_MIN_WIDTH  480

/* Maximum width (in pixels) of the wout window. Long lines clip
 * at the right edge rather than growing the window past the
 * screen. Larger than WNPT_MAX_WIDTH because wout commonly shows
 * pre-formatted output like `git log` or `man` excerpts.
 * Default: 1600. */
#define WOUT_MAX_WIDTH  1600

/* Maximum height (in pixels) of the wout window. If the piped
 * input has more lines than fit, rows past this height clip
 * silently — there is no scrolling. Pipe through `head` if you
 * only want the top of a long file.
 * Default: 1000. */
#define WOUT_MAX_HEIGHT 1000

#endif /* WLNCH_CONFIG_H */
