{
  description = "wlnch: tiny Wayland launcher, prompt (wlnch-in) and viewer (wlnch-out)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      inherit (nixpkgs) lib;
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = f: lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
      version = "0-unstable-${self.shortRev or self.dirtyShortRev or "dirty"}";
      mkWlnch = pkgs: pkgs.callPackage ./nix/package.nix { inherit version; };
    in
    {
      packages = forAllSystems (pkgs: rec {
        wlnch = mkWlnch pkgs;
        default = wlnch;
      });

      overlays.default = final: _prev: { wlnch = mkWlnch final; };

      nixosModules = rec {
        wlnch =
          { pkgs, ... }:
          {
            imports = [ ./nix/module.nix ];
            programs.wlnch.package = lib.mkDefault self.packages.${pkgs.stdenv.hostPlatform.system}.wlnch;
          };
        default = wlnch;
      };

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell { inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.wlnch ]; };
      });

      formatter = forAllSystems (pkgs: pkgs.nixfmt);

      checks = forAllSystems (
        pkgs:
        let
          inherit (pkgs.stdenv.hostPlatform) system;
          eval = lib.nixosSystem {
            inherit system;
            modules = [
              self.nixosModules.default
              {
                boot.isContainer = true;
                system.stateVersion = "25.11";
                programs.wlnch = {
                  enable = true;
                  theme = {
                    cornerRadius = 4;
                    font = "monospace:size=20";
                    colors.key = "FFFFB86C";
                  };
                  menus.launcher = {
                    entries = [
                      {
                        key = "f";
                        name = "firefox";
                        command = "firefox";
                      }
                      {
                        key = "o";
                        name = "term";
                        command = "kitty";
                        sticky = true;
                      }
                      "---"
                      {
                        key = "l";
                        name = "lock";
                        command = "swaylock";
                        color = "#E06B6B";
                      }
                    ];
                    extraConfig = "q:quit:true";
                  };
                };
              }
            ];
          };
          inherit (eval.config.programs) wlnch;
          failed = lib.filter (a: !a.assertion) eval.config.assertions;
        in
        {
          package = self.packages.${system}.wlnch;
          module =
            assert lib.assertMsg (failed == [ ]) (lib.concatMapStringsSep "\n" (a: a.message) failed);
            pkgs.runCommand "wlnch-module-check" { } ''
              menu=${wlnch.menus.launcher.package}/bin/launcher
              test -x $menu
              head -n1 $menu | grep -qx '#!${wlnch.finalPackage}/bin/wlnch'
              grep -qx 'f:firefox:firefox' $menu
              grep -qx 'o&:term:kitty' $menu
              grep -qx -- '---' $menu
              grep -qx 'l#E06B6B:lock:swaylock' $menu
              grep -qx 'q:quit:true' $menu
              test ${wlnch.finalPackage} != ${self.packages.${system}.wlnch}
              for b in wlnch wlnch-in wlnch-out; do test -x ${wlnch.finalPackage}/bin/$b; done
              touch $out
            '';
        }
      );
    };
}
