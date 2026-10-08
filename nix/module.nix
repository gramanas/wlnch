{
  config,
  lib,
  pkgs,
  ...
}:

let
  inherit (lib) mkOption mkEnableOption types;

  cfg = config.programs.wlnch;

  intOpt =
    macro: description:
    mkOption {
      type = types.nullOr types.ints.unsigned;
      default = null;
      description = "${description} (`${macro}` in config.h). `null` keeps the built-in default.";
    };

  colorOpt =
    macro: description:
    mkOption {
      type = types.nullOr (types.strMatching "[0-9a-fA-F]{8}");
      default = null;
      example = "F0242424";
      description = "${description} as `AARRGGBB` hex (`${macro}` in config.h). `null` keeps the built-in default.";
    };

  # option path (relative to cfg.theme) -> config.h macro, with a value formatter
  themeMacros =
    let
      int = v: toString v;
      color = v: "0x${lib.toUpper v}U";
      str = v: builtins.toJSON v;
    in
    [
      [
        [ "font" ]
        "DEFAULT_FONT"
        str
      ]
      [
        [ "fontPixel" ]
        "DEFAULT_FONT_PIXEL"
        int
      ]
      [
        [
          "colors"
          "bg"
        ]
        "COLOR_BG"
        color
      ]
      [
        [
          "colors"
          "fg"
        ]
        "COLOR_FG"
        color
      ]
      [
        [
          "colors"
          "key"
        ]
        "COLOR_KEY"
        color
      ]
      [
        [
          "colors"
          "keySticky"
        ]
        "COLOR_KEY_STICKY"
        color
      ]
      [
        [
          "colors"
          "sep"
        ]
        "COLOR_SEP"
        color
      ]
      [
        [
          "colors"
          "cursor"
        ]
        "CURSOR_COLOR"
        color
      ]
      [
        [
          "colors"
          "prompt"
        ]
        "COLOR_PROMPT"
        color
      ]
      [
        [ "paddingX" ]
        "PADDING_X"
        int
      ]
      [
        [ "paddingY" ]
        "PADDING_Y"
        int
      ]
      [
        [ "rowGap" ]
        "ROW_GAP"
        int
      ]
      [
        [ "keyGap" ]
        "KEY_GAP"
        int
      ]
      [
        [ "cornerRadius" ]
        "CORNER_RADIUS"
        int
      ]
      [
        [ "cursorWidth" ]
        "CURSOR_WIDTH"
        int
      ]
      [
        [
          "input"
          "minWidth"
        ]
        "WLNCH_IN_MIN_WIDTH"
        int
      ]
      [
        [
          "input"
          "maxWidth"
        ]
        "WLNCH_IN_MAX_WIDTH"
        int
      ]
      [
        [
          "output"
          "minWidth"
        ]
        "WLNCH_OUT_MIN_WIDTH"
        int
      ]
      [
        [
          "output"
          "maxWidth"
        ]
        "WLNCH_OUT_MAX_WIDTH"
        int
      ]
      [
        [
          "output"
          "maxHeight"
        ]
        "WLNCH_OUT_MAX_HEIGHT"
        int
      ]
    ];

  configH = lib.listToAttrs (
    lib.concatMap (
      m:
      let
        value = lib.attrByPath (lib.elemAt m 0) null cfg.theme;
      in
      lib.optional (value != null) (lib.nameValuePair (lib.elemAt m 1) ((lib.elemAt m 2) value))
    ) themeMacros
  );

  entryType = types.submodule {
    options = {
      key = mkOption {
        # One character; ASCII-ness is checked by an assertion below since
        # Nix regexes are byte-based and can't count UTF-8 codepoints.
        type = types.strMatching "[^#:[:space:]]+";
        example = "f";
        description = "Single (case-sensitive) character that triggers the entry.";
      };
      name = mkOption {
        type = types.strMatching "[^:\n]+";
        description = "Label shown next to the key.";
      };
      command = mkOption {
        type = types.strMatching "[^\n]+";
        description = "Shell command to run.";
      };
      sticky = mkOption {
        type = types.bool;
        default = false;
        description = "Keep wlnch open after running the command (`KEY&`).";
      };
      color = mkOption {
        type = types.nullOr (types.strMatching "#?[0-9a-fA-F]{6}");
        default = null;
        example = "#FF8800";
        description = "Key letter color override (`KEY#RRGGBB`). Not allowed with `sticky`.";
      };
    };
  };

  renderEntry =
    e:
    if lib.isString e then
      e
    else
      let
        modifier =
          if e.sticky then
            "&"
          else if e.color != null then
            "#" + lib.removePrefix "#" e.color
          else
            "";
      in
      "${e.key}${modifier}:${e.name}:${e.command}";

  menuScript =
    name: entries: extraConfig:
    pkgs.writeTextFile {
      inherit name;
      executable = true;
      destination = "/bin/${name}";
      text = ''
        #!${lib.getExe cfg.finalPackage}
        ${lib.concatMapStrings (e: renderEntry e + "\n") entries}${extraConfig}
      '';
    };

  allEntries = lib.concatMap (
    m:
    lib.map (e: {
      menu = m.name;
      entry = e;
    }) (lib.filter lib.isAttrs m.value.entries)
  ) (lib.attrsToList cfg.menus);
in
{
  options.programs.wlnch = {
    enable = mkEnableOption "wlnch, wlnch-in and wlnch-out";

    package = mkOption {
      type = types.package;
      default = pkgs.callPackage ./package.nix { };
      defaultText = lib.literalMD "built with the system's `pkgs`";
      description = "The wlnch package. Theme options are applied on top of it via `override`.";
    };

    font = mkOption {
      type = types.nullOr types.str;
      default = null;
      example = "Iosevka:size=24";
      description = ''
        Runtime font (fontconfig pattern), exported as `WLNCH_FONT` for all
        three tools. Unlike `theme.font` this does not trigger a rebuild.
      '';
    };

    theme = {
      font = mkOption {
        type = types.nullOr types.str;
        default = null;
        example = "Iosevka:size=24:weight=bold";
        description = "Compiled-in default font (`DEFAULT_FONT`). `null` keeps the built-in default.";
      };
      fontPixel = intOpt "DEFAULT_FONT_PIXEL" "Fallback pixel size when the font pattern has no :size=";
      colors = {
        bg = colorOpt "COLOR_BG" "Window background";
        fg = colorOpt "COLOR_FG" "Entry name / text color";
        key = colorOpt "COLOR_KEY" "Key letter color";
        keySticky = colorOpt "COLOR_KEY_STICKY" "Key letter color of sticky entries";
        sep = colorOpt "COLOR_SEP" "Bracket color around the key";
        cursor = colorOpt "CURSOR_COLOR" "wlnch-in cursor color";
        prompt = colorOpt "COLOR_PROMPT" "wlnch-in prompt color";
      };
      paddingX = intOpt "PADDING_X" "Horizontal window padding";
      paddingY = intOpt "PADDING_Y" "Vertical window padding";
      rowGap = intOpt "ROW_GAP" "Extra gap between rows";
      keyGap = intOpt "KEY_GAP" "Gap between the key column and the names";
      cornerRadius = intOpt "CORNER_RADIUS" "Window corner radius";
      cursorWidth = intOpt "CURSOR_WIDTH" "wlnch-in cursor width";
      input = {
        minWidth = intOpt "WLNCH_IN_MIN_WIDTH" "wlnch-in minimum window width";
        maxWidth = intOpt "WLNCH_IN_MAX_WIDTH" "wlnch-in maximum window width";
      };
      output = {
        minWidth = intOpt "WLNCH_OUT_MIN_WIDTH" "wlnch-out minimum window width";
        maxWidth = intOpt "WLNCH_OUT_MAX_WIDTH" "wlnch-out maximum window width";
        maxHeight = intOpt "WLNCH_OUT_MAX_HEIGHT" "wlnch-out maximum window height";
      };
    };

    finalPackage = mkOption {
      type = types.package;
      readOnly = true;
      default = if configH == { } then cfg.package else cfg.package.override { inherit configH; };
      defaultText = lib.literalMD "`package` with the `theme` options applied";
      description = "The package actually installed, after applying `theme`.";
    };

    menus = mkOption {
      default = { };
      description = ''
        Launcher menus. Each `menus.<name>` becomes an executable `<name>` on
        PATH: a wlnch config file with a `#!…/bin/wlnch` shebang. Bind it to a
        key in your compositor.
      '';
      example = lib.literalExpression ''
        {
          launcher.entries = [
            { key = "f"; name = "firefox"; command = "firefox"; }
            { key = "o"; name = "term"; command = "kitty"; sticky = true; }
            "---"
            { key = "l"; name = "lock"; command = "swaylock"; color = "#E06B6B"; }
          ];
        }
      '';
      type = types.attrsOf (
        types.submodule (
          { name, config, ... }:
          {
            options = {
              entries = mkOption {
                type = types.listOf (types.either (types.enum [ "---" ]) entryType);
                default = [ ];
                description = ''Menu entries, in order. The string `"---"` inserts a visual separator.'';
              };
              extraConfig = mkOption {
                type = types.lines;
                default = "";
                description = "Raw `KEY[&|#RRGGBB]:NAME:COMMAND` lines appended after `entries`.";
              };
              package = mkOption {
                type = types.package;
                readOnly = true;
                default = menuScript name config.entries config.extraConfig;
                defaultText = lib.literalMD "generated";
                description = "The generated menu executable, e.g. for `\${config.programs.wlnch.menus.launcher.package}/bin/launcher`.";
              };
            };
          }
        )
      );
    };
  };

  config = lib.mkIf cfg.enable {
    assertions =
      lib.map (name: {
        assertion =
          !(lib.elem name [
            "wlnch"
            "wlnch-in"
            "wlnch-out"
          ]);
        message = "programs.wlnch.menus.${name}: name clashes with a wlnch binary.";
      }) (lib.attrNames cfg.menus)
      ++ lib.concatMap (
        { menu, entry }:
        [
          {
            assertion = !(entry.sticky && entry.color != null);
            message = "programs.wlnch.menus.${menu}: entry '${entry.key}' cannot be both sticky and colored.";
          }
          {
            assertion = builtins.match "[ -~]*" entry.key == null || lib.stringLength entry.key == 1;
            message = "programs.wlnch.menus.${menu}: key '${entry.key}' must be a single character.";
          }
        ]
      ) allEntries;

    environment.systemPackages = [ cfg.finalPackage ] ++ lib.mapAttrsToList (_: m: m.package) cfg.menus;

    environment.variables = lib.mkIf (cfg.font != null) { WLNCH_FONT = cfg.font; };

    # The compiled-in default font is "liberation mono".
    fonts.packages = lib.mkIf (cfg.font == null && cfg.theme.font == null) [ pkgs.liberation_ttf ];
  };
}
