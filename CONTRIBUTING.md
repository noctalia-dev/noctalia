Contributing
===

This file collects contributor-facing details for Noctalia: design goals, stack notes, code style, source layout,
runtime asset behavior, and debugging helpers.

For dependencies and normal build commands, start with [README.md](README.md).

Before contributing, read our [ethos](https://noctalia.dev/ethos) to understand the values and philosophy guiding the
project.

## Design Principles

- Direct Wayland + OpenGL ES only -- no toolkit overhead
- Minimal scene graph, domain-specific to shell UI
- Packaging should work across all major Linux distros: Arch, NixOS, Fedora, Gentoo, Debian, Void, OpenSuse

## Stack

| Layer | Library |
|-------|---------|
| Wayland core | `libwayland-client`, `wayland-scanner`, `wayland-protocols` |
| Surfaces | `xdg-shell`, `zwlr-layer-shell-v1` |
| Multi-monitor | `zxdg-output-unstable-v1` |
| Active window metadata | `zwlr-foreign-toplevel-management-unstable-v1` |
| Workspaces | `ext-workspace-v1`, `dwl-ipc-unstable-v2` |
| Clipboard | `ext-data-control-v1`, `wlr-data-control-unstable-v1` |
| Activation | `xdg-activation-v1` |
| Lockscreen | `ext-session-lock-v1` |
| Idle | `ext-idle-notify-v1`, `idle-inhibit-unstable-v1` |
| Cursor | `wp-cursor-shape-v1` |
| Keyboard | `xkbcommon` |
| Rendering | `EGL`, `OpenGL ES 2.0+`, `wayland-egl` |
| Text | `cairo`, `pango`, `pangocairo`, `freetype`, `fontconfig` |
| Images | `Wuffs` (vendored), `stb_image_resize2`, `libwebp`, `librsvg` |
| IPC | `sdbus-c++` |
| Audio | `libpipewire`, `dr_wav` (vendored) |
| Authentication | `PAM`, `polkit-agent-1`, `polkit-gobject-1`, `glib-2.0`, `gobject-2.0` |
| HTTP | `libcurl` |
| Config | `tomlplusplus` |
| JSON | `nlohmann/json` |
| Math expressions | `libqalculate` |
| Scripting | `Luau` (vendored) |
| Theme generation | Material Color Utilities (vendored) |

## Runtime Assets

`meson install` installs the binary and shipped assets separately using the normal prefix layout:

```text
/usr/local/bin/noctalia
/usr/local/share/noctalia/assets/...
```

With a different Meson `prefix`/`datadir`, the same structure is preserved under that prefix.

Noctalia needs the `assets/` tree at runtime. Copying only the bare `noctalia` binary is not enough.

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

Runtime asset lookup order:

1. `NOCTALIA_ASSETS_DIR`
2. `assets/` next to the executable
3. `assets/` one level above the executable
4. install-style `../share/noctalia/assets` relative to the executable
5. the compiled install path from Meson (`<prefix>/<datadir>/noctalia/assets`)
6. the source-tree `assets/` directory as a development fallback

An asset root is only accepted if it contains the expected shipped files such as `emoji.json`, `fonts/tabler.ttf`,
`templates/builtin.toml`, and `translations/en.json`.

## Code Style

This project uses [clang-format](https://clang.llvm.org/docs/ClangFormat.html) for formatting. Run `just format`
before committing.

For editor integration, `just configure` creates a root `compile_commands.json` symlink to the selected Meson build
directory. Run `just configure`, `just configure release`, or `just configure asan` for the build mode you want clangd
to use.

The repo also includes `lefthook.yml`. Run `lefthook install` to install the pre-commit hook; it runs `just format`
before commits and refreshes the git index for tracked formatting changes.

### Naming Conventions

| | Convention | Example |
|---|---|---|
| Files | snake_case | `widget_factory.cpp` |
| Directories | snake_case | `shell/bar/widgets/` |
| Types / Classes | PascalCase | `WidgetFactory` |
| Functions / Methods | camelCase | `createWidget()` |
| Variables / Parameters | camelCase | `busName` |
| Private members | m_camelCase | `m_changeCallback` |
| Macros / Enum values | SCREAMING_SNAKE_CASE | `MAX_SIZE` |

D-Bus wire-protocol string literals, such as `player["bus_name"]`, stay snake_case because they are wire names, not
C++ identifiers.

## Translations

Noctalia translations are managed through [Noctalia Translate](https://i18n.noctalia.dev/projects/noctalia). The JSON
files in `assets/translations/` are exported from that workflow, with `assets/translations/en.json` acting as the
source catalog for new strings.

When a code, UI, settings, or documentation change needs a new user-facing string, add or update the English string in
`assets/translations/en.json` only. Do not machine translate strings, copy English into other locales, or include broad
updates to non-English translation files in a normal feature or bug-fix PR. The translation team handles those locale
updates through the translation app.

Only edit non-English translation files when the PR is explicitly about translation tooling, an import/export sync, or a
maintainer has asked for that specific locale change.

After adding or renaming translation keys, run:

```sh
python3 tools/i18n-check.py
```

## Project Layout

```text
src/
  main.cpp          Entry point
  app/              Application bootstrap, main loop, poll sources
  auth/             PAM and fingerprint authentication
  calendar/         CalDAV, Google Calendar, iCalendar parsing, polling
  capture/          Screenshots, screencopy capture, region overlay
  compositors/      Compositor detection, runtime adapters, workspace/output/keyboard backends
  config/           Configuration schema, validation, hot reload, state store, overrides
    schema/         Typed config schema engine
  core/             Logging, timers, process helpers, resource paths, shared utilities
  dbus/             Session/system bus wrappers and service integrations
    accounts/       AccountsService user metadata
    bluetooth/      BlueZ service and pairing agent
    idle/           Screensaver D-Bus service
    logind/         logind integration
    mpris/          Media player integration and artwork cache
    network/        NetworkManager, wpa_supplicant, and secret agent integration
    notification/   Desktop notification D-Bus service
    polkit/         Polkit authentication agent
    power/          power-profiles-daemon integration
    tray/           StatusNotifierItem watcher/host
    upower/         Battery and power device integration
  debug/            Debug D-Bus service
  hooks/            User hook state and hook manager
  i18n/             Translation catalog and language tag handling
  idle/             Idle manager, inhibitor, grace overlay
  ipc/              IPC client/service and CLI command parsing
  launcher/         Launcher providers (apps, emoji, calculator, sessions, windows, plugins)
  net/              HTTP client, URI parsing, URL opening
  notification/     Notification model, manager, filtering, history
  pipewire/         PipeWire audio service, sound playback, spectrum analyzer
  render/
    animation/      Animation manager, easing, motion settings
    backend/        GLES render backend, framebuffers, texture manager
    core/           Render data types, image decoders/encoders, texture caches
    programs/       GLES shader programs
    scene/          Scene graph nodes and pointer input dispatch
    text/           Cairo/Pango text and glyph rendering
  scripting/        Luau plugin runtime, manifests, registry, source management, bindings
  shell/
    backdrop/       Backdrop layer surfaces
    bar/            Bar surface, instance, widget base/factory
      widgets/      Bar widget implementations
    clipboard/      Clipboard history panel and paste helpers
    control_center/ Control center panel, tabs, shortcut registry
    desktop/        Desktop widget host, factory, layout, editor support
      widgets/      Desktop widget implementations
    dock/           Dock surface, model, pinned apps, context menu
    greeter/        Greeter appearance sync
    launcher/       Launcher panel
    lockscreen/     Session lock surfaces and lockscreen widgets
    notification/   Notification toasts
    osd/            On-screen display overlays
    overview/       Overview capture helpers
    panel/          Panel base, manager, attached-panel context
    polkit/         Polkit prompt panel
    screen_corners/ Screen corner overlays
    session/        Session panel and action runners
    settings/       Settings window, setting controls, registries, popups
    setup_wizard/   First-run setup wizard
    surface/        Shared shell surface geometry and shadow helpers
    switcher/       Window switcher UI
    tooltip/        Tooltip manager
    tray/           Tray drawer and D-Bus menu UI
    wallpaper/      Wallpaper surfaces, paths, picker panel
    widgets_editor/ Background widget editor
  system/           Desktop entries, brightness, weather, location, system monitor, hardware services
  theme/            Palette generation, templates, theme service, template application
  time/             Time service and polling
  ui/
    controls/       Reusable controls (Button, Input, Label, Select, Slider, Box, ...)
    dialogs/        File, color, and glyph picker dialogs
    visuals/        Shared visualizer controls
  util/             Generic helpers
  wayland/          Wayland connection, seats, surfaces, clipboard, toplevels, text input
    hyprland/       Hyprland-specific Wayland protocol helpers
assets/
  fonts/            Bundled Tabler and UI fonts
  sounds/           Notification and UI sounds
  templates/        Built-in theme templates
  translations/     Exported translation catalogs
protocols/          Vendored Wayland protocol XML files
tests/              Unit tests and config validation fixtures
tools/              Developer and translation helper scripts
nix/                Nix package, module, and dev shell definitions
third_party/
  wuffs/          Raster image decoding (vendored)
  dr_wav/         WAV decoder (vendored)
  fzy/            Fuzzy matching (vendored)
  luau/           Plugin scripting runtime (vendored)
  material_color_utilities/ Material Design color generation (vendored)
```

## Debugging

All debug commands use the `dev.noctalia.Debug` D-Bus service, available at runtime.

```sh
# Enable verbose debug logs
gdbus call --session --dest dev.noctalia.Debug --object-path /dev/noctalia/Debug --method dev.noctalia.Debug.SetVerboseLogs true

# Disable verbose debug logs
gdbus call --session --dest dev.noctalia.Debug --object-path /dev/noctalia/Debug --method dev.noctalia.Debug.SetVerboseLogs false

# Check current verbose log state
gdbus call --session --dest dev.noctalia.Debug --object-path /dev/noctalia/Debug --method dev.noctalia.Debug.GetVerboseLogs

# Emit an internal notification (app_name, summary, body, timeout_ms, urgency 0-2)
gdbus call --session --dest dev.noctalia.Debug --object-path /dev/noctalia/Debug --method dev.noctalia.Debug.EmitInternalNotification "Noctalia" "Test" "Hello from debug" 5000 1
```
