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
  libjxl,
  libsndfile,
  glib,
  polkit,
  librsvg,
  libqalculate,
  libxml2,
  md4c,
  libsecret,
  libsodium,
  stb,
  fetchFromGitHub,
  nlohmann_json,
  tomlplusplus,
  libical,
  wireplumber,
  jemalloc,
  makeWrapper,
  git,
  gsettings-desktop-schemas,
  autoAddDriverRunpath,
  rev ? "unknown",
  # DEPRECATED: no longer affects the build; kept for `.override` compat.
  cudaSupport ? config.cudaSupport,
}:
let
  version = lib.fileContents ../VERSION;
  stb' = stb.overrideAttrs (_: {
    version = "unstable-2025-10-26";
    src = fetchFromGitHub {
      owner = "nothings";
      repo = "stb";
      rev = "f1c79c02822848a9bed4315b12c8c8f3761e1296";
      hash = "sha256-BlyXJtAI7WqXCTT3ylww8zoG0hBxaojJnQDvdQOXJPE=";
    };
  });
in
lib.warnIf cudaSupport
  "nythera: `cudaSupport` no longer has any effect (autoAddDriverRunpath is now always applied); this argument will be removed in the future."
  stdenv.mkDerivation {
  pname = "nythera";
  inherit version;

  src = lib.cleanSource ./..;

  postPatch = ''
    substituteInPlace meson.build \
      --replace-fail "_git_revision_config.set('VCS_TAG', 'unknown')" "_git_revision_config.set('VCS_TAG', '${rev}')"
  '';

  postFixup = ''
    wrapProgram $out/bin/nythera \
      --prefix PATH : ${lib.makeBinPath [ git ]} \
      --prefix XDG_DATA_DIRS : "${glib.getSchemaDataDirPath gsettings-desktop-schemas}"

    $out/bin/nythera completions bash | install -D /dev/stdin $out/share/bash-completion/completions/nythera
    $out/bin/nythera completions zsh  | install -D /dev/stdin $out/share/zsh/site-functions/_nythera
    $out/bin/nythera completions fish | install -D /dev/stdin $out/share/fish/vendor_completions.d/nythera.fish
  '';

  nativeBuildInputs = [
    meson
    ninja
    pkg-config
    wayland-scanner
    jemalloc
    makeWrapper
    autoAddDriverRunpath
  ];

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
    libjxl
    libsndfile
    glib
    polkit
    librsvg
    libqalculate
    libxml2
    md4c
    libsecret
    libsodium
    stb'
    nlohmann_json
    tomlplusplus
    libical
  ];

  mesonBuildType = "release";

  mesonFlags = [ "-Dtests=disabled" ];

  ninjaFlags = [ "-v" ];

  meta = with lib; {
    description = "A sleek, customizable desktop shell crafted for Wayland.";
    homepage = "https://github.com/Nythera-Dev/Nythera";
    license = licenses.mit;
    platforms = platforms.linux;
    mainProgram = "nythera";
  };
}
