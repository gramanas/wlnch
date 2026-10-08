{
  lib,
  stdenv,
  pkg-config,
  wayland,
  wayland-scanner,
  libxkbcommon,
  freetype,
  fontconfig,
  version ? "unstable",
  # Overrides for the compile-time defaults in config.h, as an attrset of
  # macro name -> C expression, e.g. { CORNER_RADIUS = "4"; COLOR_BG = "0xFF000000U"; }.
  # Every name must already be #define'd in config.h; unknown names fail the build.
  configH ? { },
}:

let
  overrideMacro = name: value: ''
    N=${lib.escapeShellArg name} V=${lib.escapeShellArg value} awk '
      BEGIN { n = ENVIRON["N"]; v = ENVIRON["V"] }
      $1 == "#define" && $2 == n { print "#define " n " " v; found = 1; next }
      { print }
      END { if (!found) exit 1 }
    ' config.h > config.h.new || {
      echo "configH: macro ${name} is not defined in config.h" >&2
      exit 1
    }
    mv config.h.new config.h
  '';
in
stdenv.mkDerivation {
  pname = "wlnch";
  inherit version;

  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../Makefile
      ../config.h
      ../wlnch.c
      ../wlnch-in.c
      ../wlnch-out.c
      ../wlnch.el
      ../wlnchrc.example
      ../protocols
    ];
  };

  strictDeps = true;

  nativeBuildInputs = [
    pkg-config
    wayland-scanner
  ];

  buildInputs = [
    wayland
    libxkbcommon
    freetype
    fontconfig
  ];

  postPatch = lib.concatStrings (lib.mapAttrsToList overrideMacro configH);

  makeFlags = [ "PREFIX=${placeholder "out"}" ];

  postInstall = ''
    install -Dm644 wlnch.el -t $out/share/emacs/site-lisp
    install -Dm644 wlnchrc.example -t $out/share/doc/wlnch
  '';

  meta = {
    description = "Tiny Wayland command launcher, prompt and viewer (wlnch, wlnch-in, wlnch-out)";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
    mainProgram = "wlnch";
  };
}
