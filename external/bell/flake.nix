{
  description = "bell - Various internal CPP utilities";

  inputs = {
    nixpkgs.url = "nixpkgs/nixos-24.11";
    nixpkgs-unstable.url = "github:nixos/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = {
    self,
    nixpkgs,
    nixpkgs-unstable,
    flake-utils,
  }: let
    overlay = final: prev: {
      unstable = nixpkgs-unstable.legacyPackages.${prev.system};
    };
  in
    {
      overlays.default = overlay;
    }
    // flake-utils.lib.eachDefaultSystem (system: let
      pkgs = import nixpkgs {
        inherit system;
        overlays = [overlay];
      };

      llvm = pkgs.llvmPackages_19;

      common-pkgs = with pkgs;
            [
              cmake
              ninja
              clang-tools
              avahi
            ];

      apps = {
      };

      packages = {
        tests = llvm.stdenv.mkDerivation {
          name = "tests";
          src = ./.;
          cmakeFlags = ["-DBELL_DISABLE_TESTS=OFF" "-DBELL_DISABLE_MDNS=ON"];
          nativeBuildInputs = common-pkgs;
          enableParallelBuilding = true;
          doCheck = true;

          # Run the unit tests
          checkPhase = ''
            ./test/bell-test
          '';
        };
      };

      devShells = {
        default = pkgs.mkShell {
          packages = common-pkgs;
        };
      };
    in {
      inherit apps devShells packages;
      checks = packages;
      devShell = devShells.default;
    });
}
