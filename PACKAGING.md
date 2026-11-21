# Packaging Nythera

Nythera is an experimental fork of [Noctalia](https://github.com/noctalia-dev/noctalia). It does **not** currently claim the distribution packaging or release support of the upstream project.

## Current packaging status

No official Nythera packages or stable release policy are established yet. See [BUILDING.md](BUILDING.md) for source builds.

## Current technical identity

The executable, desktop entries, installation paths, environment variables, completions and application-owned IPC identifiers use Nythera. Upstream-hosted services remain external dependencies; see [REBRAND.md](REBRAND.md) for migration and integration details.

### Existing upstream identifiers (pending rename)

|                 |                                                                                                                |
| --------------- | -------------------------------------------------------------------------------------------------------------- |
| Name            | `nythera`                                                                                                     |
| Homepage        | <https://github.com/Nythera-Dev/Nythera>                                                                    |
| Docs            | <https://docs.noctalia.dev>                                                                                    |
| License         | MIT ([LICENSE](LICENSE)); also see vendored licenses under `third_party/` for SPDX completeness                |
| Version         | [`VERSION`](VERSION), read by Meson `project(... version: files('VERSION'))`                                   |
| Binary          | `nythera`                                                                                                     |
| Desktop entries | `dev.nythera.Nythera.desktop`; `dev.nythera.Nythera.Annotator.desktop` (hidden, image "Open With" handler) |
| Icon            | `nythera` (`share/icons/hicolor/scalable/apps/nythera.svg`)                                                  |


## Architectures / libc

First-class targets:

- `x86_64` (amd64)
- `aarch64` (arm64)

Linux only. There is no intentional arch-specific code path; other little-endian Linux arches may build if dependencies are available, but they are not first-class supported.

**musl**: known to build (e.g. Alpine / Void musl). Not first-class tested. `jemalloc` is glibc-only, on musl leave `-Djemalloc=disabled` (or `auto`, which skips it).

## Compilers / linkers

C++23 is required.

| Toolchain                   | Minimum       | Notes                                                                            |
| --------------------------- | ------------- | -------------------------------------------------------------------------------- |
| GCC + libstdc++             | **GCC 13+**   | Default on most distros                                                          |
| Clang + libstdc++ or libc++ | **Clang 16+** | libc++ needs Meson’s experimental-library flags (already wired in `meson.build`) |

Debian 12 “bookworm” needs a newer toolchain than the default `g++` (e.g. `g++-13` with `CXX=g++-13`).

No special linker is required. The default linker from the compiler driver is fine (**GNU ld** / gold, **lld**, **mold** via `-fuse-ld=` all work). Release builds pass `-Wl,--gc-sections` and `-Wl,--as-needed`, which those linkers support.

## Build

- Build system: [Meson](https://mesonbuild.com/) + Ninja. The repo’s [`justfile`](justfile) is convenience only; packaging can call Meson directly.
- Language / toolchain: see [Compilers / linkers](#compilers--linkers).
- Recommended: `meson setup build --buildtype=release`, then compile/install.
- Do **not** enable `-Dnative_optimizations=true` for distro packages (CPU-local codegen; not portable).
- Tests: `-Dtests=disabled` (or leave `auto`, which skips tests for release).
- `jemalloc`: recommended on glibc; Meson feature option `-Djemalloc=auto|enabled|disabled`. Only used on glibc builds.
- `system_libraries`: `-Dsystem_libraries=true` links system copies of the vendored libraries instead of building `third_party/`. See [Vendored](#vendored-no-system-package) for the expected layout.

Prefix/datadir are baked into the binary via `NYTHERA_INSTALL_PREFIX` / `NYTHERA_INSTALL_DATADIR`. Install with the same prefix you configured.

### Installed layout

```text
<prefix>/bin/nythera
<prefix>/share/nythera/assets/...
<prefix>/share/applications/dev.nythera.Nythera.desktop
<prefix>/share/applications/dev.nythera.Nythera.Annotator.desktop
<prefix>/share/icons/hicolor/scalable/apps/nythera.svg
```

The shipped `assets/` tree is **required at runtime**. Shipping only the binary breaks fonts, translations, templates, glyphs, and sounds.

### Runtime asset lookup

Besides the prefix layout above, portable bundle layouts are supported:

```text
bundle/
  nythera
  assets/
```

```text
bundle/
  bin/nythera
  share/nythera/assets/
```

The current implementation resolves the asset root in this order:

1. `NYTHERA_ASSETS_DIR` (a warning is logged if it is set but invalid)
2. `assets/` next to the executable
3. `assets/` one level above the executable (this is how a `build-debug/` binary finds the source tree's `assets/`)
4. `../<datadir>/nythera/assets` relative to the executable, when the configured Meson `datadir` is relative
5. `../share/nythera/assets` relative to the executable
6. the compiled install path from Meson (`<prefix>/<datadir>/nythera/assets`)

A candidate is only accepted if it contains `emoji.json`, `fonts/nythera-tabler.ttf`, `templates/builtin.toml`, and `translations/en.json`. If none qualifies, The current implementation logs a warning and uses the compiled install path.

Not shipped (don’t look for them in the install): AppStream / metainfo XML, man pages, systemd units.

### Shell completions

Static completion files are not shipped. Generate them from the freshly built binary and install them in the distribution's completion directories:

```text
./build-<mode>/nythera completions bash  → share/bash-completion/completions/nythera
./build-<mode>/nythera completions zsh   → share/zsh/site-functions/_nythera
./build-<mode>/nythera completions fish  → share/fish/vendor_completions.d/nythera.fish
```

This is intentionally a packaging step rather than a Meson install rule: executing the target binary during installation would break cross-compilation.

## Dependencies

No Qt or GTK. UI is Wayland + OpenGL ES (EGL/GLES, or `libepoxy` as a fallback when separate EGL/GLES packages are missing).

### Build-time (required)

Canonical list is the `dependency(...)` / header checks in [`meson.build`](meson.build). Distro package names differ; [BUILDING.md](BUILDING.md) has copy-paste install lines for common distros.

Notes packagers hit often:

- **stb** must provide `stb/stb_image_resize2.h` (and `stb/stb_image_write.h`). Older packages that only ship `stb_image_resize` are not enough. Meson fails the configure check if `stb_image_resize2` is missing.
- Meson requires **WirePlumber 0.5** (`wireplumber-0.5` pkg-config). 0.4 is not enough.

### Vendored (no system package)

Shipped under `third_party/`: Wuffs, Luau, fzy, Material Color Utilities. Each carries its own license file beside the code.

With `-Dsystem_libraries=true`, Meson uses system copies of all four instead; the option is all-or-nothing. None of them ship pkg-config files, so Meson finds them by library and header name on the compiler search paths:

| Library                  | Libraries                                                                             | Headers                          |
| ------------------------ | ------------------------------------------------------------------------------------- | -------------------------------- |
| Wuffs                    | none (single-file release, compiled into Nythera)                                    | `wuffs-v0.4.c`                   |
| fzy                      | `libfzy` (built from `src/match.c`)                                                   | `fzy/match.h`                    |
| Material Color Utilities | `libmaterial-color-utilities` (C++ implementation)                                    | `cpp/...` (upstream layout)      |
| Luau                     | `libLuau.Compiler`, `libLuau.Bytecode`, `libLuau.Ast`, `libLuau.VM`, `libLuau.Common` | `lua.h`, `lualib.h`, `luacode.h` |

Vendored versions: Wuffs 0.4.0-alpha.9, Luau 0.742 (0.724 also builds), fzy 1.1, Material Color Utilities C++ at upstream commit `1a34bd2` with abseil replaced by the standard library and missing standard includes added. The public headers do not use abseil, so a system copy built with or without abseil links the same way. Plugins only get the Luau language features of the linked release.

### Runtime

| Dependency                              | Role                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                |
| --------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| PipeWire **daemon** (+ WirePlumber 0.5) | Audio, volume OSD, privacy indicators, spectrum. Libraries alone are not enough; without a running daemon those features stay off.                                                                                                                                                                                                                                                                                                                                                                                  |
| PAM (`login` service by default)        | Lock screen authentication                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| Fontconfig / fonts                      | Text rendering (users still need usable fonts installed)                                                                                                                                                                                                                                                                                                                                                                                                                                                            |
| `git`                                   | Plugin git sources / auto-update invoke `git` on `PATH`                                                                                                                                                                                                                                                                                                                                                                                                                                                             |
| `upower`                                | Optional: battery / power devices                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| `power-profiles-daemon`                 | Optional: power profile switching                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| `ddcutil`                               | Optional: external monitor brightness                                                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| Secret Service provider                 | Optional but recommended for credential / encrypted-state persistence (GNOME Keyring, KWallet, KeePassXC, ...). `libsecret` is only the client library; without a session provider those features cannot persist secrets.                                                                                                                                                                                                                                                                                           |
| `sound-theme-freedesktop`               | Shell sounds                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        |
| `libheif` (`libheif.so.1`)              | Optional but recommended: HEIC, HEIF, and AVIF decoding for wallpapers, thumbnails, clipboard previews, and notification images. Loaded with `dlopen` on first use, so it is neither a build dependency nor a link dependency; declare it as a weak dependency (Arch `optdepends`, Debian/Fedora `Recommends`). AVIF needs libheif's dav1d or aom decoder plugin; HEIC needs an HEVC decoder plugin such as libde265. Fedora's main `libheif` lacks the HEVC decoder; RPM Fusion's `libheif-freeworld` provides it. |

## Startup and IPC

No systemd user unit is shipped. Typical startup is compositor autostart or the desktop entry (`nythera --daemon`). Control a running instance with `nythera msg ...` (Unix socket under `XDG_RUNTIME_DIR`).

Keep the `.desktop` daemon Exec unless you intentionally package a different session integration. Prefer the canonical description above for `Comment=` / AppStream summary as well.

## Session conflicts

On non-Plasma sessions The current implementation provides and registers:

- `org.freedesktop.Notifications`
- `org.kde.StatusNotifierWatcher` (system tray host)

Do not run it alongside another notification daemon or StatusNotifier host (mako, dunst, swaync, waybar-as-host, ...) unless those are disabled. On Plasma, the shell integrates with Plasma's notification / tray paths instead of claiming the freedesktop Notifications name.

## User data paths

| Kind   | Default                                                                              |
| ------ | ------------------------------------------------------------------------------------ |
| Config | `$XDG_CONFIG_HOME/nythera` (`~/.config/nythera`), e.g. `config.toml`               |
| State  | `$XDG_STATE_HOME/nythera` (`~/.local/state/nythera`), e.g. `settings.toml`, caches |
| Data   | `$XDG_DATA_HOME/nythera` (`~/.local/share/nythera`)                                |
| Logs   | `$XDG_CACHE_HOME/nythera` (`~/.cache/nythera`)                                     |

Override bases with `NYTHERA_CONFIG_HOME`, `NYTHERA_STATE_HOME`, `NYTHERA_DATA_HOME` (each still appends `/nythera`).


## Project scope

Nythera is a Wayland desktop shell, not a compositor or display manager. Its packaging, naming and compatibility may change while development continues.

## Versioning and contact

No stable Nythera release or packaging maintenance policy has been set. Report Nythera-specific issues at <https://github.com/Nythera-Dev/Nythera/issues>. Upstream Noctalia support channels are for the upstream project.
