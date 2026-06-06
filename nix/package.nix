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
  jemalloc,
  autoAddDriverRunpath,
  cudaSupport ? config.cudaSupport,
}:
let
  inherit (builtins) head match readFile;
  version = head (match ".*version: '([^']+)'.*" (readFile ../meson.build));
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
    pam
    curl
    libwebp
    glib
    polkit
    librsvg
    libqalculate
    libxml2
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
