# wlnch

A tiny Wayland command launcher written in C, with no GUI toolkit. It opens an
unmanaged overlay surface, grabs the keyboard, shows your configured keybinds,
runs the chosen command, and exits.

It's a Wayland counterpart to `xlnch` and uses the same `KEY:NAME:COMMAND`
config format.

This repository also builds two sibling utilities that share the same
overlay-window look:

- [`wlnch-in`](#wlnch-in) — a minimal note prompt with readline-style line
  editing. Reads typed text from the keyboard and prints it to stdout
  when the user presses Enter.
- [`wlnch-out`](#wlnch-out) — a stdin viewer. Slurps stdin, displays it in the
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

### Nix / NixOS

The repo is a flake:

```sh
nix build            # result/bin/{wlnch,wlnch-in,wlnch-out}
nix run . -- path/to/wlnchrc
nix develop          # shell with the build deps, then `make`
```

For NixOS, add the flake as an input and import its module:

```nix
{
  inputs.wlnch.url = "git+ssh://git@legit.eyesin.space/grm/wlnch";

  outputs = { nixpkgs, wlnch, ... }: {
    nixosConfigurations.myhost = nixpkgs.lib.nixosSystem {
      modules = [ wlnch.nixosModules.default ./configuration.nix ];
    };
  };
}
```

```nix
# configuration.nix
programs.wlnch = {
  enable = true;

  # Compile-time theme (config.h); unset options keep the defaults.
  theme = {
    font = "Iosevka:size=24";
    cornerRadius = 8;
    colors.key = "FFFFB86C";   # AARRGGBB
  };

  # Each menu becomes an executable on PATH (here: `launcher`).
  menus.launcher.entries = [
    { key = "f"; name = "firefox"; command = "firefox"; }
    { key = "o"; name = "term"; command = "kitty"; sticky = true; }
    "---"
    { key = "l"; name = "lock"; command = "swaylock"; color = "#E06B6B"; }
  ];
};
```

Bind `launcher` (or `${config.programs.wlnch.menus.launcher.package}/bin/launcher`)
to a key in your compositor. Other options: `font` (runtime `WLNCH_FONT`,
no rebuild), `menus.<name>.extraConfig` (raw config lines), and
`theme.input.*` / `theme.output.*` for the `wlnch-in` / `wlnch-out` window
sizes. The package is also available via `wlnch.overlays.default`, and
`pkgs.wlnch.override { configH = { CORNER_RADIUS = "4"; }; }` sets any
`config.h` macro directly.

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

### Emacs major mode

[`wlnch.el`](wlnch.el) provides a minor-overhead major mode for editing
config files in Emacs:

- `#`-prefixed lines render as comments; the `#!` shebang on line 1
  gets a distinct face.
- `---` separator lines are highlighted.
- `#RRGGBB` modifiers preview their colour inline (the hex value is
  used as the background, with a black or white foreground chosen for
  contrast — same trick as `css-mode`).
- The `COMMAND` portion of every entry is fontified as Bash by
  piggy-backing on `sh-mode`, so quoting, variable expansion, and
  redirection all light up the way you'd expect.

It auto-activates for files named `wlnchrc`, `.wlnchrc`, or
`wlnchrc.<anything>`, and for any file whose shebang interpreter is
`wlnch`. Drop it on your `load-path` and `(require 'wlnch)`.

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

## wlnch-in

`wlnch-in` ("wlnch input") is a companion binary built from the same
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

Pasting from the system clipboards:

- `Ctrl+V` — paste from the **clipboard** (the Wayland
  `wl_data_device` selection — what other apps' Ctrl+C writes into)
- `Shift+Insert` — paste from the **primary selection** (the Wayland
  `zwp_primary_selection_device_v1` buffer — what middle-click pastes
  from)

Pasted text is inserted at the cursor as-is; embedded newlines stay
in the buffer rather than committing (`Enter` is reserved for
explicit submission). NUL bytes and `\r` are stripped, so CRLF input
is normalised to LF. wlnch-in prefers `text/plain;charset=utf-8` and
falls back through `UTF8_STRING`, `text/plain`, then `STRING`/`TEXT`.

Both bindings no-op silently if the compositor doesn't support the
relevant manager (clipboard via `wl_data_device_manager`, primary via
`zwp_primary_selection_device_manager_v1`).

Typical usage:

```sh
wlnch-in > note.txt                     # capture a quick note to a file
echo "hello $(wlnch-in)"                # interpolate a typed value into a command
wlnch-in -p "title: " > new-post.md     # show a labeled prompt before the input
```

Visual styling (font, colors, padding, corner radius, cursor, prompt
color) is shared with `wlnch` via `config.h`. The font can also be
overridden per-run with `-f FONT` or via `$WLNCH_IN_FONT` (falling back to
`$WLNCH_FONT`).

The `-p PROMPT` flag draws a single-line label in front of the input
area, rendered in `COLOR_PROMPT` (defaults to the same accent blue
`wlnch` uses for keys). The prompt is purely visual — it never enters
the buffer and is never written to stdout. Multi-line prompts are
rejected with a clear error.

## wlnch-out

`wlnch-out` ("wlnch output") is the third sibling. It reads all of stdin into
memory at startup, opens an overlay layer-surface that looks like
`wlnch` / `wlnch-in`, renders the text statically, and exits when the user
dismisses the window. There is no editing, no cursor, no scrolling —
strictly a "show this and wait" dialog.

Dismiss any of: `Esc`, `Enter`, `q`, `Space`, `Ctrl+G`.

Typical usage:

```sh
echo "build finished"     | wlnch-out
git log --oneline -10     | wlnch-out
date                      | wlnch-out

# show the result of a long-running command when it's done
( make 2>&1; echo "exit=$?" ) | wlnch-out

# auto-close after 3 seconds (toast-style notification)
echo "saved!"             | wlnch-out -t 3000
date '+%H:%M:%S'          | wlnch-out --timeout 1500
```

Flags:

- `-f FONT` / `--font FONT` — fontconfig pattern (env: `$WLNCH_OUT_FONT`,
  then `$WLNCH_FONT`).
- `-t MS` / `--timeout MS` — auto-close after `MS` milliseconds. The
  default `0` means "no timeout"; the window stays open until the
  user dismisses it.

`wlnch-out` refuses to read from a tty (so a bare `wlnch-out` doesn't silently
hang on terminal stdin); use a pipe or a redirection.

Sizing:

- Width grows to fit the widest line, clamped to
  `[WLNCH_OUT_MIN_WIDTH, WLNCH_OUT_MAX_WIDTH]`.
- Height grows linearly with line count, clamped to
  `WLNCH_OUT_MAX_HEIGHT`.
- Long lines clip at the right edge; rows past the height cap clip
  at the bottom. Pipe through `head` / `cut` if you only want the
  beginning of a long file.

Visual styling and the height/width caps are tunable in
[`config.h`](config.h). The font can also be overridden per-run with
`-f FONT` or via `$WLNCH_OUT_FONT` (falling back to `$WLNCH_FONT`).

## Why not GNOME?

The Wayland design has no equivalent of X11's override-redirect. The only
standard way to create an unmanaged surface that can grab the keyboard is the
`wlr-layer-shell` protocol, which Mutter has consistently declined to
implement. Supporting GNOME would require falling back to `xdg-shell`, where
the compositor manages the window and there is no focus-grab guarantee, so
`wlnch` would no longer behave like a launcher.
