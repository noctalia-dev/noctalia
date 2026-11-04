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

    # Build tools invoked directly by just recipes
    meson
    ninja
    pkg-config
    wayland-scanner

    # Formatting (required by justfile)
    llvmPackages_23.clang-tools
    llvmPackages_23.libclang
    gnugrep
    gnused
    findutils
    python3

    # Debugging
    gdb
  ];

  shellHook = ''
    # Point to local assets so binaries find resources without installation
    export NOCTALIA_ASSETS_DIR="$PWD/assets"

    echo " Noctalia dev-shell | 'just --list' to see available tasks"
  '';
}
