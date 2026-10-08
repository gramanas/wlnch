# Integrating wlnch into a NixOS configuration

Instructions for an agent editing a flake-based NixOS configuration to
install and configure **wlnch** (Wayland launcher), **wlnch-in** (text
prompt → stdout) and **wlnch-out** (stdin → overlay viewer). No
home-manager is needed: everything is a NixOS module.

Requirements: a compositor that supports `wlr-layer-shell` (Sway, Hyprland,
river, niri, Wayfire, KDE Plasma). GNOME is **not** supported.

## 1. Add the flake input

In the config's `flake.nix`:

```nix
inputs = {
  # ...existing inputs...
  wlnch = {
    url = "git+https://legit.eyesin.space/grm/wlnch";
    inputs.nixpkgs.follows = "nixpkgs";
  };
};
```

Make sure `wlnch` is in the `outputs` argument set (or `inputs` is passed via
`specialArgs`), then add the module to every host that should have it:

```nix
nixosConfigurations.<host> = nixpkgs.lib.nixosSystem {
  # ...
  modules = [
    inputs.wlnch.nixosModules.default
    # ...existing modules...
  ];
};
```

Then run `nix flake lock --update-input wlnch` (or `nix flake update wlnch`)
so `flake.lock` gets the entry. Commit/`git add` the lock file. Flakes only
see tracked files.

Do **not** also add `inputs.wlnch.overlays.default` unless something else
needs `pkgs.wlnch`; the module already builds the package with the system's
`pkgs`, so its libraries (fontconfig etc.) always match the running system.

## 2. Configure `programs.wlnch`

Minimal:

```nix
programs.wlnch.enable = true;
```

This installs `wlnch`, `wlnch-in` and `wlnch-out` system-wide.

Full example:

```nix
{ config, ... }:
{
  programs.wlnch = {
    enable = true;

    # Runtime font for all three tools (sets WLNCH_FONT, no rebuild).
    # font = "Iosevka:size=24";

    # Compile-time theme. Every option defaults to null = keep built-in value.
    # Setting any of them rebuilds the package locally (small C build, ~seconds).
    theme = {
      font = "Iosevka:size=24";          # fontconfig pattern
      cornerRadius = 8;
      paddingX = 24;
      colors = {
        bg = "F0242424";                 # AARRGGBB, no "#" / "0x"
        fg = "FFF6F3E8";
        key = "FF8AB4F8";
        keySticky = "FFE06B6B";
      };
    };

    # Each attribute becomes an executable on PATH with that name.
    menus.launcher.entries = [
      { key = "f"; name = "firefox"; command = "firefox"; }
      { key = "t"; name = "terminal"; command = "foot"; sticky = true; }
      "---"                                            # visual separator
      { key = "l"; name = "lock"; command = "swaylock -f"; color = "#E06B6B"; }
      { key = "n"; name = "note"; command = "wlnch-in -p 'note: ' >> ~/notes.txt"; }
    ];

    menus.power.entries = [
      { key = "s"; name = "suspend"; command = "systemctl suspend"; }
      { key = "r"; name = "reboot"; command = "systemctl reboot"; }
      { key = "p"; name = "poweroff"; command = "systemctl poweroff"; }
    ];
  };
}
```

## 3. Bind menus in the compositor

A menu is just a command, so bind it like any other. Prefer the store path
from the read-only `package` option so the binding never depends on PATH:

```nix
# Sway via NixOS (programs.sway) / or wherever the sway config is generated:
"${config.programs.wlnch.menus.launcher.package}/bin/launcher"
```

Plain-text examples:

```
# sway / i3-style
bindsym $mod+d exec launcher
bindsym $mod+Shift+e exec power

# Hyprland
bind = SUPER, D, exec, launcher

# niri (KDL)
Mod+D { spawn "launcher"; }
```

If the compositor config is a raw file in the repo, use the bare command name
(`launcher`). It is in `/run/current-system/sw/bin`.

## Option reference (`programs.wlnch.*`)

| Option | Type | Default | Notes |
|---|---|---|---|
| `enable` | bool | `false` | Installs the three binaries. |
| `package` | package | built from the system's `pkgs` | Base package; theme is applied on top. |
| `finalPackage` | package (read-only) | | Package actually installed. |
| `font` | null or str | `null` | Exported as `WLNCH_FONT`. Runtime, no rebuild. |
| `theme.font` | null or str | `null` → `"liberation mono:size=32"` | Compiled-in default font. |
| `theme.fontPixel` | null or uint | `null` → 32 | Fallback size if the pattern has no `:size=`. |
| `theme.colors.{bg,fg,key,keySticky,sep,cursor,prompt}` | null or `"AARRGGBB"` | `null` | 8 hex digits, alpha first. `cursor` / `prompt` affect wlnch-in. |
| `theme.{paddingX,paddingY,rowGap,keyGap,cornerRadius,cursorWidth}` | null or uint | `null` | Pixels. |
| `theme.input.{minWidth,maxWidth}` | null or uint | `null` → 480 / 1200 | wlnch-in window width. |
| `theme.output.{minWidth,maxWidth,maxHeight}` | null or uint | `null` → 480 / 1600 / 1000 | wlnch-out window size. |
| `menus.<name>.entries` | list of entry or `"---"` | `[]` | See below. |
| `menus.<name>.extraConfig` | lines | `""` | Raw config lines appended after `entries`. |
| `menus.<name>.package` | package (read-only) | | Contains `bin/<name>`. |

Entry attributes:

| Attr | Required | Rules |
|---|---|---|
| `key` | yes | Exactly one character, case-sensitive (uppercase = Shift). Not `#`, `:` or whitespace. |
| `name` | yes | Label. Must not contain `:` or newlines. |
| `command` | yes | Run via the shell. May contain `:`. No newlines. |
| `sticky` | no (`false`) | Run the command but keep the menu open. |
| `color` | no (`null`) | `"#RRGGBB"` key-letter color. Cannot be combined with `sticky`. |

Raw config line format (for `extraConfig`): `KEY[&|#RRGGBB]:NAME:COMMAND`.

## Rules and gotchas

- **Menu names must not be `wlnch`, `wlnch-in` or `wlnch-out`.** An assertion
  rejects them. Also avoid names that clash with other packages' binaries.
- Duplicate keys within one menu are not rejected; the first match wins. Keep
  keys unique.
- If neither `font` nor `theme.font` is set, the module adds
  `pkgs.liberation_ttf` to `fonts.packages` (the built-in default font). If you
  set a font, make sure that font is installed (`fonts.packages`).
- The old names `wnpt` / `wout` and the env vars `WNPT_FONT` / `WOUT_FONT` no
  longer exist. Use `wlnch-in` / `wlnch-out` and `WLNCH_IN_FONT` /
  `WLNCH_OUT_FONT`. Replace any old references in the config (keybindings,
  scripts).
- Useful one-liners for commands/scripts:
  - `wlnch-in -p "prompt: "` prints the typed text to stdout. Exit 1 on Esc.
  - `cmd | wlnch-out` shows the output. `wlnch-out -t 3000` auto-closes after 3 s.

## Verify

```sh
nix flake check                                   # in the nixos-config repo
nixos-rebuild build --flake .#<host>              # builds without switching
cat result/sw/bin/launcher                        # shebang + KEY:NAME:COMMAND lines
nix eval .#nixosConfigurations.<host>.config.programs.wlnch.finalPackage
```

Then `sudo nixos-rebuild switch --flake .#<host>` (let the user run this), and
press the bound key in the compositor.
