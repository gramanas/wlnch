# wlnch

A tiny Wayland command launcher written in C, with no GUI toolkit. It opens an
unmanaged overlay surface, grabs the keyboard, shows your configured keybinds,
runs the chosen command, and exits.

It's a Wayland counterpart to `xlnch` and uses the same `KEY:NAME:COMMAND`
config format.

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

## Why not GNOME?

The Wayland design has no equivalent of X11's override-redirect. The only
standard way to create an unmanaged surface that can grab the keyboard is the
`wlr-layer-shell` protocol, which Mutter has consistently declined to
implement. Supporting GNOME would require falling back to `xdg-shell`, where
the compositor manages the window and there is no focus-grab guarantee, so
`wlnch` would no longer behave like a launcher.
