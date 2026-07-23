# Packaging Noctalia

Notes for distribution packagers. End-user install docs live in the
[README](README.md) and at [docs.noctalia.dev](https://docs.noctalia.dev/v5/getting-started/installation).

## Package description

Use this exact short description for package metadata (`pkgdesc`, `Summary`,
AppStream, etc.):

> A sleek, customizable desktop shell crafted for Wayland.

Do not substitute shorter or alternate blurbs (“lightweight Wayland bar”,
“status bar”, ...). Noctalia is a full desktop shell (bars, dock, launcher,
notifications, lock screen, wallpaper, settings, ...), not a bar-only tool.

## Packaged distributions

v5 is already packaged for:

- Arch Linux
- NixOS
- Fedora
- openSUSE
- KaOS
- Gentoo
- Void Linux
- GNU Guix
- Debian (including Ubuntu)

[Repology](https://repology.org/project/noctalia/versions) is a useful at-a-glance
check, but it only covers repositories it indexes (and may still list v4 as
`noctalia-shell`). It is not a complete inventory of community packaging.

## Identity

| | |
|---|---|
| Name | `noctalia` |
| Homepage | https://github.com/noctalia-dev/noctalia |
| Docs | https://docs.noctalia.dev |
| License | MIT ([LICENSE](LICENSE)); also see vendored licenses under `third_party/` for SPDX completeness |
| Version | Meson `project(... version: ...)` in [`meson.build`](meson.build) |
| Binary | `noctalia` |
| Desktop entry | `dev.noctalia.Noctalia.desktop` |
| Icon | `noctalia` (`share/icons/hicolor/scalable/apps/noctalia.svg`) |

## Architectures / libc

First-class targets:

- `x86_64` (amd64)
- `aarch64` (arm64)

Linux only. There is no intentional arch-specific code path; other little-endian
Linux arches may build if dependencies are available, but they are not
first-class supported.

**musl**: known to build (e.g. Alpine / Void musl). Not first-class tested.
`jemalloc` is glibc-only, on musl leave `-Djemalloc=disabled` (or `auto`, which
skips it).

## Compilers / linkers

C++23 is required.

| Toolchain | Minimum | Notes |
|---|---|---|
| GCC + libstdc++ | **GCC 13+** | Default on most distros |
| Clang + libstdc++ or libc++ | **Clang 16+** | libc++ needs Meson’s experimental-library flags (already wired in `meson.build`) |

Debian 12 “bookworm” needs a newer toolchain than the default `g++` (e.g.
`g++-13` with `CXX=g++-13`).

No special linker is required. The default linker from the compiler driver is
fine (**GNU ld** / gold, **lld**, **mold** via `-fuse-ld=` all work). Release
builds pass `-Wl,--gc-sections` and `-Wl,--as-needed`, which those linkers
support.

## Build

- Build system: [Meson](https://mesonbuild.com/) + Ninja. The repo’s
  [`justfile`](justfile) is convenience only; packaging can call Meson
  directly.
- Language / toolchain: see [Compilers / linkers](#compilers--linkers).
- Recommended: `meson setup build --buildtype=release`, then compile/install.
- Do **not** enable `-Dnative_optimizations=true` for distro packages (CPU-local
  codegen; not portable).
- Tests: `-Dtests=disabled` (or leave `auto`, which skips tests for release).
- `jemalloc`: recommended on glibc; Meson feature option `-Djemalloc=auto|enabled|disabled`.
  Only used on glibc builds.

Prefix/datadir are baked into the binary via `NOCTALIA_INSTALL_PREFIX` /
`NOCTALIA_INSTALL_DATADIR`. Install with the same prefix you configured.

### Installed layout

```text
<prefix>/bin/noctalia
<prefix>/share/noctalia/assets/...
<prefix>/share/applications/dev.noctalia.Noctalia.desktop
<prefix>/share/icons/hicolor/scalable/apps/noctalia.svg
```

The shipped `assets/` tree is **required at runtime**. Shipping only the binary
breaks fonts, translations, templates, glyphs, and sounds. See
[CONTRIBUTING.md](CONTRIBUTING.md#runtime-assets) for lookup order (including
`NOCTALIA_ASSETS_DIR` overrides for unusual layouts).

Not shipped (don’t look for them in the install): AppStream / metainfo XML, man
pages, systemd units.

## Dependencies

No Qt or GTK. UI is Wayland + OpenGL ES (EGL/GLES, or `libepoxy` as a fallback
when separate EGL/GLES packages are missing).

### Build-time (required)

Canonical list is the `dependency(...)` / header checks in
[`meson.build`](meson.build). Distro package names differ; the [README](README.md)
has copy-paste install lines for common distros.

Notes packagers hit often:

- **stb** must provide `stb/stb_image_resize2.h` (and `stb/stb_image_write.h`).
  Older packages that only ship `stb_image_resize` are not enough. Meson fails
  the configure check if `stb_image_resize2` is missing.
- Meson requires **WirePlumber 0.5** (`wireplumber-0.5` pkg-config). 0.4 is not
  enough.

### Vendored (no system package)

Shipped under `third_party/`: Wuffs, Luau, dr_wav, fzy, Material Color Utilities.
Each carries its own license file beside the code.

### Runtime

| Dependency | Role |
|---|---|
| PipeWire **daemon** (+ WirePlumber 0.5) | Audio, volume OSD, privacy indicators, spectrum. Libraries alone are not enough; without a running daemon those features stay off. |
| PAM (`login` service by default) | Lock screen authentication |
| Fontconfig / fonts | Text rendering (users still need usable fonts installed) |
| `git` | Plugin git sources / auto-update invoke `git` on `PATH` |
| `upower` | Optional: battery / power devices |
| `ddcutil` | Optional: external monitor brightness |
| Secret Service provider | Optional but recommended for credential / encrypted-state persistence (GNOME Keyring, KWallet, KeePassXC, ...). `libsecret` is only the client library; without a session provider those features cannot persist secrets. |

## Startup and IPC

No systemd user unit is shipped. Typical startup is compositor autostart or the
desktop entry (`noctalia --daemon`). Control a running instance with
`noctalia msg ...` (Unix socket under `XDG_RUNTIME_DIR`).

Keep the `.desktop` daemon Exec unless you intentionally package a different
session integration. Prefer the canonical description above for `Comment=` /
AppStream summary as well.

## Session conflicts

On non-Plasma sessions Noctalia provides and registers:

- `org.freedesktop.Notifications`
- `org.kde.StatusNotifierWatcher` (system tray host)

Do not run it alongside another notification daemon or StatusNotifier host
(mako, dunst, swaync, waybar-as-host, ...) unless those are disabled. On Plasma,
Noctalia integrates with Plasma's notification / tray paths instead of claiming
the freedesktop Notifications name.

## User data paths

| Kind | Default |
|---|---|
| Config | `$XDG_CONFIG_HOME/noctalia` (`~/.config/noctalia`), e.g. `config.toml` |
| State | `$XDG_STATE_HOME/noctalia` (`~/.local/state/noctalia`), e.g. `settings.toml`, caches |
| Data | `$XDG_DATA_HOME/noctalia` (`~/.local/share/noctalia`) |
| Logs | `$XDG_CACHE_HOME/noctalia` (`~/.cache/noctalia`) |

Override bases with `NOCTALIA_CONFIG_HOME`, `NOCTALIA_STATE_HOME`,
`NOCTALIA_DATA_HOME` (each still appends `/noctalia`).

## What Noctalia is not

- Not a compositor, display manager, or greeter. Greeter support is
  [noctalia-greeter](https://github.com/noctalia-dev/noctalia-greeter).
- Not a replacement for file managers, screen casting, or drive mounting.
- Compositor support varies (protocols / IPC). See
  [compositor docs](https://docs.noctalia.dev/v5/compositor-settings/).

## Versioning and beta

v5 is currently beta. Prefer packaging **tagged releases** rather than random
`main` snapshots unless you maintain a `-git` / nightly package on purpose.

## Contact

- Issues: https://github.com/noctalia-dev/noctalia/issues
- Discord: https://discord.noctalia.dev
