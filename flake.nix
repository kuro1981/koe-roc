{
  description = "PlatformIO development environment for koe-roc";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs {
          inherit system;
        };
      in
      {
        devShells.default = pkgs.mkShell {
          buildInputs = with pkgs; [
            platformio
            git
            linuxPackages.usbip
          ];

          shellHook = ''
            echo "========================================="
            echo " koe-roc PlatformIO Development Shell"
            echo "========================================="
            echo "Available commands:"
            echo "  pio --help      - PlatformIO CLI"
            echo "  pio project init - Initialize project"
            echo "========================================="
          '';
        };
      }
    );
}
