{
  lib,
  config,
  stdenv,
  meson,
  ninja,
  pkg-config,
  wayland-scanner,
  wayland,
  wayland-protocols,
  libGL,
  libglvnd,
  freetype,
  fontconfig,
  cairo,
  pango,
  harfbuzz,
  libxkbcommon,
  sdbus-cpp_2,
  systemd,
  pipewire,
  pam,
  curl,
  libwebp,
  glib,
  polkit,
  librsvg,
  libqalculate,
  libxml2,
  wireplumber,
  jemalloc,
  md4c,
  nlohmann_json,
  tomlplusplus,
  stb,
  fetchFromGitHub,
  autoAddDriverRunpath,
  cudaSupport ? config.cudaSupport,
}:
let
  inherit (builtins) head match readFile;
  version = head (match ".*version: '([^']+)'.*" (readFile ../meson.build));

  # nixpkgs pins stb to 2023-01-29, which predates stb_image_resize2.h
  # (the v2 resize API that meson.build requires via cc.has_header).
  # Pin to the snapshot targeted by NixOS/nixpkgs#491159 so this matches
  # upstream nixpkgs once that bump lands and the override can be dropped.
  stb' = stb.overrideAttrs (_: {
    version = "0-unstable-2025-10-26";
    src = fetchFromGitHub {
      owner = "nothings";
      repo = "stb";
      rev = "f1c79c02822848a9bed4315b12c8c8f3761e1296";
      hash = "sha256-BlyXJtAI7WqXCTT3ylww8zoG0hBxaojJnQDvdQOXJPE=";
    };
  });
in
stdenv.mkDerivation {
  pname = "noctalia";
  inherit version;

  src = lib.cleanSource ./..;

  postPatch = ''
    # Remove -march=native and -mtune=native for reproducible builds
    sed -i "s/'-march=native', '-mtune=native',//" meson.build
  '';

  nativeBuildInputs = [
    meson
    ninja
    pkg-config
    wayland-scanner
    jemalloc
  ]
  ++ lib.optional cudaSupport autoAddDriverRunpath;

  buildInputs = [
    wayland
    wayland-protocols
    libGL
    libglvnd
    freetype
    fontconfig
    cairo
    pango
    harfbuzz
    libxkbcommon
    sdbus-cpp_2
    systemd
    pipewire
    wireplumber
    pam
    curl
    libwebp
    glib
    polkit
    librsvg
    libqalculate
    libxml2
    md4c
    nlohmann_json
    tomlplusplus
    stb'
  ];

  mesonBuildType = "release";

  ninjaFlags = [ "-v" ];

  meta = with lib; {
    description = "A lightweight Wayland shell and bar built directly on Wayland + OpenGL ES";
    homepage = "https://github.com/noctalia-dev/noctalia-shell";
    license = licenses.mit;
    platforms = platforms.linux;
    mainProgram = "noctalia";
  };
}
