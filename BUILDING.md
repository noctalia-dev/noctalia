# Building Noctalia from source

This guide covers source dependencies, distro-specific package installation, build modes, and install layouts.
For prebuilt packages and other installation methods, see the
[installation documentation](https://docs.noctalia.dev/noctalia/getting-started/installation/).

## Dependencies

### Arch

```sh
sudo pacman -S meson gcc just \
  wayland wayland-protocols \
  libglvnd freetype2 fontconfig \
  cairo pango harfbuzz \
  libxkbcommon glib2 \
  libsecret libsodium \
  sdbus-cpp libpipewire wireplumber polkit \
  pam curl libwebp libjxl libsndfile librsvg \
  libqalculate libxml2 \
  md4c tomlplusplus libical \
  nlohmann-json stb \
  jemalloc
```

### Fedora

```sh
sudo dnf install meson gcc-c++ just \
  wayland-devel wayland-protocols-devel \
  libEGL-devel mesa-libGLES-devel \
  freetype-devel fontconfig-devel \
  cairo-devel pango-devel harfbuzz-devel \
  libxkbcommon-devel glib2-devel \
  libsecret-devel libsodium-devel \
  sdbus-cpp-devel pipewire-devel wireplumber-devel \
  pam-devel polkit-devel libcurl-devel libwebp-devel libjxl-devel libsndfile-devel librsvg2-devel \
  libqalculate-devel libxml2-devel \
  md4c-devel tomlplusplus-devel libical-devel \
  json-devel stb_image_resize2-devel stb_image_write-devel \
  jemalloc-devel
```

### openSUSE Tumbleweed / Slowroll

```sh
sudo zypper install meson gcc-c++ just \
  wayland-devel wayland-protocols-devel \
  Mesa-libEGL-devel Mesa-libGLESv2-devel \
  freetype2-devel fontconfig-devel \
  cairo-devel pango-devel harfbuzz-devel \
  libxkbcommon-devel glib2-devel \
  libsecret-devel libsodium-devel \
  sdbus-cpp-devel pipewire-devel wireplumber-devel \
  pam-devel polkit-devel libcurl-devel libwebp-devel libjxl-devel libsndfile-devel librsvg-devel \
  libqalculate-devel libxml2-devel \
  md4c-devel tomlplusplus-devel libical-devel \
  nlohmann_json-devel stb-devel \
  jemalloc-devel
```

### Debian / Ubuntu

```sh
sudo apt install meson g++ just \
  libwayland-dev wayland-protocols \
  libegl-dev libgles-dev \
  libfreetype-dev libfontconfig-dev \
  libcairo2-dev libpango1.0-dev libharfbuzz-dev \
  libxkbcommon-dev libglib2.0-dev \
  libsecret-1-dev libsodium-dev \
  libsdbus-c++-dev libpipewire-0.3-dev libwireplumber-0.5-dev \
  libpam0g-dev libpolkit-agent-1-dev libpolkit-gobject-1-dev \
  libcurl4-openssl-dev libwebp-dev libjxl-dev libsndfile1-dev librsvg2-dev \
  libqalculate-dev libxml2-dev \
  libmd4c-dev libtomlplusplus-dev libical-dev \
  nlohmann-json3-dev libstb-dev \
  libjemalloc-dev
```

### Void Linux

```sh
sudo xbps-install meson ninja pkg-config git \
  wayland-devel wayland-protocols libepoxy-devel \
  MesaLib-devel libglvnd-devel cairo-devel \
  pango-devel fontconfig-devel freetype-devel \
  harfbuzz-devel libxkbcommon-devel pipewire-devel wireplumber-devel \
  libsecret-devel libsodium-devel \
  libcurl-devel pam-devel libwebp-devel libjxl-devel libsndfile-devel \
  basu-devel sdbus-c++-devel \
  libmd4c-devel tomlplusplus-devel libical-devel \
  json-c++ stb \
  polkit-devel librsvg-devel libqalculate-devel libxml2-devel jemalloc-devel
```

### AerynOS

```sh
sudo moss install meson gcc clang just \
  wayland-devel wayland-protocols-devel \
  libglvnd-devel freetype-devel fontconfig-devel \
  cairo-devel pango-devel harfbuzz-devel \
  libxkbcommon-devel glib2-devel \
  libsecret-devel libsodium-devel \
  sdbus-cpp-devel pipewire-devel wireplumber-devel polkit-devel \
  linux-pam-devel curl libwebp-devel libjxl-devel libsndfile-devel librsvg-devel \
  libqalculate-devel libxml2-devel \
  md4c-devel tomlplusplus-devel libical-devel \
  nlohmann-json stb \
  jemalloc-devel
```

Vendored dependencies, with no system package needed: `Wuffs`,
`Luau`, `fzy`, and Material Color Utilities.

System packages required beyond the Wayland/GL stack: `libwebp` handles WebP decoding and thumbnail encoding,
`libjxl` handles JPEG XL decoding, `libsndfile` decodes shell sound effects (WAV, FLAC, Ogg/Vorbis, Opus, MP3, and
AIFF), and Wuffs handles the other supported raster image formats. `libqalculate` powers
the launcher calculator (arithmetic, unit and currency conversion).

Polkit agent support requires development files that provide the `polkit-agent-1` and `polkit-gobject-1` pkg-config
modules. Some distros ship these in the runtime `polkit` package, while split-package distros use names such as
`polkit-devel`, `polkit-dev`, or `libpolkit-agent-1-dev` / `libpolkit-gobject-1-dev`.

Pipewire libraries/headers are sufficient to build Noctalia, but there is also a runtime requirement for the pipewire
daemon. Noctalia will abort startup if it can't connect to the daemon. If your distro splits the pipewire libraries
and daemon into separate packages, make sure you have both installed.

`upower` is an optional dependency used for battery and power device integration.

`ddcutil` is an optional dependency used for controlling monitor brightness.

Credential and encrypted-state persistence requires a Secret Service provider at runtime, such as GNOME Keyring,
KWallet, or KeePassXC. `libsecret` is the client library and does not provide the session service by itself. Noctalia
continues to run when no provider is available, but features requiring durable secrets cannot persist them.
CalDAV accounts may instead read their password from one explicitly configured regular file, which supports secret
provisioners such as agenix and sops-nix without installing a Secret Service provider. Google refresh tokens and
other writable credentials still require Secret Service. Encrypted state, including clipboard history and the calendar
event cache, may instead read one storage master key from an explicitly configured file.

`jemalloc` is recommended but optional. It reduces memory fragmentation in long-running sessions, and on glibc systems
it is used automatically when detected. Use Meson's `-Djemalloc=enabled` or `-Djemalloc=disabled` option to require or
disable it explicitly.

Sanitizer runtime packages are only needed for ASan/UBSan builds configured with `just configure asan`.

The sources are built as C++23, which requires GCC 13+ or Clang 16+. Current rolling and recent stable distros (Arch,
Fedora 38+, Debian 13, Ubuntu 24.04+) ship a new enough compiler by default. On Debian 12 "bookworm" install `g++-13`
and point Meson at it (e.g. `CXX=g++-13 just configure`).

## Building and installing

Requires [just](https://github.com/casey/just) and [meson](https://mesonbuild.com/).

### Release build

```sh
# Optimized release build in build-release/
just configure release
just build release

# Install the selected build mode. This does not build or reconfigure.
sudo just install release
```

Release builds are portable by default. For a machine-local build, enable native CPU optimizations after configuring:

```sh
meson configure build-release -Dnative_optimizations=true
just build release
```

Pass a prefix to `configure` to install somewhere other than `/usr/local`:

```sh
just configure release "$HOME/.local"
just build release
just install release
```

To remove files installed from a build directory, run `just uninstall release`. The `install` and `uninstall` recipes
require an explicit build mode so debug builds are not installed by accident.

### Debug build

```sh
# Debug build in build-debug/ for local development and troubleshooting.
just configure
just build

# Test your local debug build with
just run
```

Unit tests are not compiled by `just build`, which targets only the Noctalia executable. Build and run them explicitly
with `just test` (use `just test release` to force them on for a release build). Direct Meson users can control test
target generation with the `-Dtests=enabled|disabled|auto` option.
Production sources compile once into an internal static library shared by the shell and test executables.

Meson installs the binary and shipped assets using the normal prefix layout:

```text
/usr/local/bin/noctalia
/usr/local/share/noctalia/assets/...
```

Noctalia needs the shipped `assets/` tree at runtime. Copying only the `noctalia` binary is not enough.

Firefox theming uses the built-in template `post_action = "firefox-theme"` (same pattern as
`kde-color-scheme`) plus the [Pywalfox](https://addons.mozilla.org/en-US/firefox/addon/pywalfox/)
browser extension. Manual host helpers: `noctalia firefox-theme --help`.

Portable bundle layouts are also supported:

```text
bundle/
  noctalia
  assets/
```

```text
bundle/
  bin/noctalia
  share/noctalia/assets/
```

See [CONTRIBUTING.md](CONTRIBUTING.md#runtime-assets) for the full runtime asset lookup order.
