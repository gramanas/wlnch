# wlnch

A tiny Wayland command launcher written in C, with no GUI toolkit. It opens an
unmanaged overlay surface, grabs the keyboard, shows your configured keybinds,
runs the chosen command, and exits.

It's a Wayland counterpart to `xlnch` and uses the same `KEY:NAME:COMMAND`
config format.

This repository also builds two sibling utilities that share the same
overlay-window look:

- [`wnpt`](#wnpt) — a minimal note prompt with readline-style line
  editing. Reads typed text from the keyboard and prints it to stdout
  when the user presses Enter.
- [`wout`](#wout) — a stdin viewer. Slurps stdin, displays it in the
  overlay, and dismisses on any of `Esc` / `Enter` / `q` / `Space` /
  `Ctrl+G`.

## Compositor support

`wlnch` requires the `wlr-layer-shell-unstable-v1` protocol so that the surface
can be unmanaged and grab the keyboard exclusively. This is supported by:

- wlroots-based compositors: Sway, Hyprland, river, niri, Wayfire, ...
- KDE Plasma (KWin)

GNOME / Mutter does **not** implement layer-shell, so `wlnch` will not run
there.

## Build

Dependencies (development packages):

- `wayland-client` and `wayland-scanner`
- `libxkbcommon`
- `freetype2`
- `fontconfig`

```sh
make
sudo make install   # installs to /usr/local/bin by default
```

Compile-time defaults (font, colors, padding, row gap, etc.) live in
[`config.h`](config.h); edit and recompile to retheme.

## Configuration

Each non-blank, non-comment line is `KEY[&]:NAME:COMMAND`:

- The first two `:` are the separators, so `COMMAND` may contain `:`.
- `KEY` is a single character, matched case-sensitively (uppercase letters
  require Shift).
- An optional `&` between the key and the first `:` marks the entry
  *sticky*: the command runs but `wlnch` stays open, so the same key can
  be pressed again (e.g. `o&:term:kitty` spawns a new terminal every
  time `o` is pressed). Sticky entries are highlighted with a reddish
  key letter in the rendered list.
- An optional `#RRGGBB` between the key and the first `:` overrides
  the key letter color (6 hex digits, alpha is always `FF`). For
  example, `f#FF8800:firefox:firefox` paints the `f` orange. Mutually
  exclusive with `&`: sticky entries always render with the sticky
  color so the visual cue can't be hidden.
- A line consisting solely of `---` inserts a blank-row visual
  separator between groups of entries. Leading, trailing, and
  consecutive `---` lines are no-ops.
- A leading `#!` shebang line is treated as a comment, so config files
  can be made executable directly.

See [`wlnchrc.example`](wlnchrc.example) for a sample.

## Usage

```sh
wlnch ~/.config/wlnch/wlnchrc      # explicit config path
wlnch < ~/.config/wlnch/wlnchrc    # config from stdin
wlnch -f "Sans 14" path/to/wlnchrc # override the font (fontconfig pattern)
```

There is no default config location. Pass the config path as a positional
argument, or pipe it on stdin. The most ergonomic option is the executable
config file:

```sh
chmod +x ~/.config/wlnch/wlnchrc
~/.config/wlnch/wlnchrc            # the kernel runs `wlnch <path>`
```

…with a `#!/usr/local/bin/wlnch` shebang at the top of `wlnchrc`. This
makes the config file itself the launcher binary; bind it to a hotkey in
your compositor and you're done.

The window appears centered, grabs the keyboard, and:

- pressing a configured key runs that command and exits;
- pressing `Esc` or `Ctrl-G` exits without running anything;
- pressing any other key is ignored.

The font can also be set via the `WLNCH_FONT` environment variable.

## wnpt

`wnpt` ("Wayland note prompt") is a companion binary built from the same
repo. It opens an overlay layer-surface that looks like a `wlnch` window
but renders no preset entries — instead it accepts arbitrary text from
the keyboard and prints the buffer to stdout on commit.

Editing is readline-style: the cursor lives at an arbitrary position
inside the buffer (not just the end), and a single-slot kill ring with
consecutive-kill accumulation supports yank.

Submission:

- typing inserts UTF-8 (your current keyboard layout is honored, including
  Shift / CapsLock / dead keys);
- `Enter` commits: the buffered text is printed to stdout, exit 0;
- `Shift+Enter` inserts a newline into the buffer;
- `Esc` or `Ctrl+G` aborts: nothing is printed, exit 1.

Cursor movement:

- `Ctrl+B` / `←` and `Ctrl+F` / `→` — one char back / forward
- `Alt+B` / `Ctrl+←` and `Alt+F` / `Ctrl+→` — one word back / forward
- `Ctrl+A` / `Home` and `Ctrl+E` / `End` — beginning / end of line
- `↑` / `↓` — previous / next line, column preserved (codepoint-counted)

Editing:

- `Backspace` / `Ctrl+H` — delete previous char
- `Delete` / `Ctrl+D` — delete next char
- `Ctrl+W` / `Ctrl+Backspace` / `Alt+Backspace` — kill previous word
- `Alt+D` — kill next word
- `Ctrl+K` — kill from cursor to end of line
- `Ctrl+U` — kill from start of line to cursor
- `Ctrl+Y` — yank (paste) the kill ring at point
- `Ctrl+T` — transpose the two chars around point

Consecutive kill commands (e.g. `Ctrl+K Ctrl+K` or
`Ctrl+W Ctrl+W`) accumulate into a single kill-ring entry, so a
following `Ctrl+Y` restores everything as one paste.

Typical usage:

```sh
wnpt > note.txt                     # capture a quick note to a file
echo "hello $(wnpt)"                # interpolate a typed value into a command
wnpt -p "title: " > new-post.md     # show a labeled prompt before the input
```

Visual styling (font, colors, padding, corner radius, cursor, prompt
color) is shared with `wlnch` via `config.h`. The font can also be
overridden per-run with `-f FONT` or via `$WNPT_FONT` (falling back to
`$WLNCH_FONT`).

The `-p PROMPT` flag draws a single-line label in front of the input
area, rendered in `COLOR_PROMPT` (defaults to the same accent blue
`wlnch` uses for keys). The prompt is purely visual — it never enters
the buffer and is never written to stdout. Multi-line prompts are
rejected with a clear error.

## wout

`wout` ("Wayland out") is the third sibling. It reads all of stdin into
memory at startup, opens an overlay layer-surface that looks like
`wlnch` / `wnpt`, renders the text statically, and exits when the user
dismisses the window. There is no editing, no cursor, no scrolling —
strictly a "show this and wait" dialog.

Dismiss any of: `Esc`, `Enter`, `q`, `Space`, `Ctrl+G`.

Typical usage:

```sh
echo "build finished"     | wout
git log --oneline -10     | wout
date                      | wout

# show the result of a long-running command when it's done
( make 2>&1; echo "exit=$?" ) | wout

# auto-close after 3 seconds (toast-style notification)
echo "saved!"             | wout -t 3000
date '+%H:%M:%S'          | wout --timeout 1500
```

Flags:

- `-f FONT` / `--font FONT` — fontconfig pattern (env: `$WOUT_FONT`,
  then `$WLNCH_FONT`).
- `-t MS` / `--timeout MS` — auto-close after `MS` milliseconds. The
  default `0` means "no timeout"; the window stays open until the
  user dismisses it.

`wout` refuses to read from a tty (so a bare `wout` doesn't silently
hang on terminal stdin); use a pipe or a redirection.

Sizing:

- Width grows to fit the widest line, clamped to
  `[WOUT_MIN_WIDTH, WOUT_MAX_WIDTH]`.
- Height grows linearly with line count, clamped to
  `WOUT_MAX_HEIGHT`.
- Long lines clip at the right edge; rows past the height cap clip
  at the bottom. Pipe through `head` / `cut` if you only want the
  beginning of a long file.

Visual styling and the height/width caps are tunable in
[`config.h`](config.h). The font can also be overridden per-run with
`-f FONT` or via `$WOUT_FONT` (falling back to `$WLNCH_FONT`).

## Why not GNOME?

The Wayland design has no equivalent of X11's override-redirect. The only
standard way to create an unmanaged surface that can grab the keyboard is the
`wlr-layer-shell` protocol, which Mutter has consistently declined to
implement. Supporting GNOME would require falling back to `xdg-shell`, where
the compositor manages the window and there is no focus-grab guarantee, so
`wlnch` would no longer behave like a launcher.
