{
  pkgs,
  noctalia,
}:
pkgs.mkShell {
  inputsFrom = [ noctalia ];

  nativeBuildInputs = with pkgs; [
    # Workflow & Hooks
    just
    lefthook

    # Formatting (required by justfile)
    llvmPackages_22.clang-tools
    llvmPackages_22.libclang
    gnugrep
    gnused
    findutils

    # Debugging
    gdb
  ];

  shellHook = ''
    # Point to local assets so binaries find resources without installation
    export NOCTALIA_ASSETS_DIR="$PWD/assets"

    echo " Noctalia dev-shell | 'just --list' to see available tasks"
  '';
}
