{
  description = "llco dev environment: clang 22 + libc++, gcc 16, liburing";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs =
    { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      llvm = pkgs.llvmPackages_22;
      # gcc16's cc-wrapper setup-hook exports CXX=g++ unconditionally, so link
      # only its /bin to keep g++ available for A/B without hijacking CXX
      gpp = pkgs.buildEnv {
        name = "gpp-only";
        paths = [ pkgs.gcc16 ];
        pathsToLink = [ "/bin" ];
      };
    in
    {
      devShells.${system}.default = llvm.libcxxStdenv.mkDerivation {
        name = "llco-dev-shell";
        nativeBuildInputs = with pkgs; [
          cmake
          gdb
          gpp
          liburing # the INTERFACE library links a bare -luring
          llvm.clang-tools
          ninja
          perf
          python3
          util-linux # taskset for the pinning in bench runs
        ];
        # NIX_ENFORCE_NO_NATIVE strips -march=native, which would skew the benches
        env.NIX_ENFORCE_NO_NATIVE = "";
      };
    };
}
