{
  description = "co a programming language";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
      pkgsFor = system: import nixpkgs { inherit system; };
    in {
      packages = forAllSystems (system:
        let
          pkgs = pkgsFor system;
          llvm = pkgs.llvmPackages_22;
        in {
          default = llvm.stdenv.mkDerivation {
            pname = "faust";
            version = "0.1.0";
            src = self;
            nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.python3 llvm.llvm ];
            buildInputs = [ llvm.llvm ];
            doCheck = true;
            checkPhase = ''
              runHook preCheck
              ctest --output-on-failure
              runHook postCheck
            '';
          };
        });

      checks = forAllSystems (system: {
        compiler = self.packages.${system}.default;
      });

      devShells = forAllSystems (system:
        let
          pkgs = pkgsFor system;
          llvm = pkgs.llvmPackages_22;
        in {
          default = pkgs.mkShell.override { stdenv = llvm.stdenv; } {
            inputsFrom = [ self.packages.${system}.default ];
            packages = [ llvm.clang-tools llvm.lld llvm.lldb ];
            LLVM_DIR = "${llvm.llvm.dev}/lib/cmake/llvm";
          };
        });
    };
}
