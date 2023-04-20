{
  description = "Redex: An Android Bytecode Optimizer";

  inputs = {
    nixpkgs.url = "nixpkgs/nixos-22.11";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem
      (system:
        let pkgs = import nixpkgs { inherit system; }; in {
          formatter = pkgs.nixpkgs-fmt;
          packages = {
            default = pkgs.stdenv.mkDerivation {
              name = "redex";
              src = ./.;

              buildInputs = with pkgs; [
                cmake
                boost
                jsoncpp
                jemalloc
                lz4
                xz
                bzip2
                libiberty
                zlib
                zip
                python3
              ];
            };
          };
        }
      );
}
