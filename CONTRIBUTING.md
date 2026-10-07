Contributing
===

This file collects contributor-facing details for Noctalia: design goals, stack notes, code style, contribution rules,
testing, commit conventions, source layout, and debugging helpers.

For dependencies and normal build commands, start with [BUILDING.md](BUILDING.md). For the architecture and the
invariants reviewers enforce (UI phases, layer boundaries, rendering, surfaces, config, plugins), read
[ARCHITECTURE.md](ARCHITECTURE.md).

Before contributing, read our [ethos](https://noctalia.dev/ethos) to understand the values and philosophy guiding the
project.

## Design Principles

- Direct Wayland + OpenGL ES only -- no toolkit overhead
- Minimal scene graph, domain-specific to shell UI
- Packaging should work across all major Linux distros: Arch, NixOS, Fedora, Gentoo, Debian, Void, OpenSuse

## Stack

Direct project dependencies are listed below; transitive dependencies are owned by their providing system packages.

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
| Rendering | `EGL`, `OpenGL ES 2.0+`, `wayland-egl` (`libepoxy` fallback) |
| Dynamic loading | `libdl` when provided separately by the platform |
| Text | `cairo`, `cairo-ft`, `pango`, `pangocairo`, `pangoft2`, `harfbuzz`, `freetype2`, `fontconfig` |
| Images | `Wuffs` (vendored), `stb_image_resize2`, `stb_image_write`, `libwebp`, `libjxl`, `libjxl_threads`, `librsvg` |
| IPC and service runtime | `sdbus-c++`, `glib-2.0`, `gobject-2.0`, `gio-2.0` |
| Audio | `libpipewire-0.3`, `wireplumber-0.5`, `libsndfile` |
| Authentication | `PAM`, `polkit-agent-1`, `polkit-gobject-1` |
| Credentials and encryption | `libsecret-1`, `libsodium` |
| HTTP | `libcurl` |
| XML | `libxml2` |
| Calendar data | `libical` |
| Config | `tomlplusplus` |
| JSON | `nlohmann/json` |
| Markdown | `md4c` |
| Fuzzy matching | `fzy` (vendored) |
| Math expressions | `libqalculate` |
| Scripting | `Luau` (vendored) |
| Theme generation | Material Color Utilities (vendored) |
| Memory allocation | `jemalloc` (optional) |

## Code Style

This project uses [clang-format](https://clang.llvm.org/docs/ClangFormat.html) 22 for formatting. Run `just format`
before committing and `just format-check` to run the same check as CI, covering both `src/` and `tests/`.
Both recipes reject other formatter major versions and print the version in use.

Install `clang22` on Arch or `clang-tools-extra` on Fedora 44. If your distribution installs Clang 22 outside the default
PATH, prepend its binary directory when running either recipe, for example
`PATH=/usr/lib/llvm22/bin:$PATH just format-check` on Arch. Keep editor formatting on Clang 22 as well.

For editor integration, `just configure` creates a root `compile_commands.json` symlink to the selected Meson build
directory. Run `just configure`, `just configure release`, or `just configure asan` for the build mode you want clangd
to use.

The repo also includes `lefthook.yml`. Run `lefthook install` to install the pre-commit hook; it runs `just format`
before commits and refreshes the git index for tracked formatting changes.

### Lint

Code must pass clang-tidy with warnings treated as errors. `just lint` checks the whole tree; while iterating, lint only
the `.cpp` files you touched:

```sh
clang-tidy -p build-debug --warnings-as-errors='*' <touched .cpp files...>
```

Headers are linted through the `.cpp` files that include them. Fix every finding before opening a pull request.

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

### Source Conventions

- `src/` is the include root; project includes are relative to it (`#include "app/application.h"`).
- A single `meson.build` lists every source directly; there are no per-directory build files.
- Headers and sources are co-located; there is no separate `include/` tree.

### Comments

Comments explain what the current code does and any non-obvious constraint a reader needs. Do not narrate history,
rejected alternatives, or "why we don't do X"; git holds that. If a comment is longer than the code it describes, cut it
down.

## Contribution Rules

### One change per pull request

Keep each pull request to a single feature, fix, or refactor. Unrelated changes, including drive-by cleanups and
formatting of untouched code, go in separate pull requests so each can be reviewed and reverted on its own.

### One canonical name, no fallbacks

Every config key, IPC name, path, and identifier has exactly one canonical name. Do not add aliases, "try name X then
name Y" resolution, backwards-compatibility shims, or silent default chains that hide misconfiguration. When input is
wrong or missing, fail or report it loudly instead of papering over it. A maintainer may explicitly request a migration
path; do not add one unprompted.

This applies to identity and configuration, not runtime resilience. Graceful degradation of an operation, such as
serving a cached last-known-good value when a network or IO call fails, is fine.

### User documentation

End-user documentation lives in [`docs/user/`](docs/user/) (MDX), with images under `docs/assets/`. Any change that adds,
removes, or changes user-facing configuration or behavior updates the relevant page in the same commit as the code.

The documentation site is generated from these files by `tools/sync-docs.sh`, which copies `docs/user/**/*.mdx` into the
docs site checkout and deletes pages that no longer exist here. Never edit the generated copies; the next sync
overwrites them.

### Vendored code

Do not patch vendored code under `third_party/` for Noctalia behavior; local edits are lost when the dependency is
refreshed. Put compatibility code in Noctalia-owned sources under `src/`, or fix the issue upstream and update the
vendored snapshot from upstream.

## Testing

Unit tests are not required for every change. Choose the verification that exercises the changed behavior most
directly, and describe it in the pull request's Testing section.

Add or modify a permanent unit test only when all of the following hold:

- It protects an observable, deterministic contract.
- A plausible future regression would have meaningful impact.
- Existing tests do not already cover the contract.
- The test would fail for a plausible broken implementation.
- The test is cheaper and more reliable than exercising the behavior in the running shell.

Unit tests are most valuable for configuration parsing and migration, persistent data, security boundaries, parsers and
decoders, non-trivial state machines, and deterministic protocol transformations.

Do not add tests for trivial forwarding, getters, constants, duplicated implementation logic, implementation details, or
visual behavior modelled through mocks. Rendering, layout, animation, Wayland lifecycle, and compositor integration
changes are verified in the real shell; do not synthesize brittle pointer-event or mock-based tests to avoid a visual
check.

For bug fixes, reproduce the bug and confirm the reproduction no longer fails. Add a regression test only when it
provides durable coverage beyond that check.

Write test checks with `TEST_CHECK` from `tests/test_check.h`, never `assert`. `assert` compiles away under `NDEBUG`, so
a release build would pass a broken test silently; `TEST_CHECK` prints the failed expression with its file and line and
exits non-zero. The header refuses to compile with `NDEBUG`, so test executables set
`override_options: ['b_ndebug=false']` in `meson.build`.

## Commit Messages

Follow the Conventional Commits style used in the history: `type(scope): summary`, or `type: summary` when no scope
helps. Use lowercase types such as `feat`, `fix`, `refactor`, `chore`, `test`, `i18n`, `ui`, or `config`, and a focused
subsystem scope when useful, for example `refactor(bar): use widget option structs`. Keep the summary concise,
imperative, and specific.

Default to the summary line alone. Add a body only when it carries information a reader would otherwise miss: a
non-obvious root cause, a constraint that forced the approach, a behavior change users will notice, or a `Fixes #N`
reference. Do not restate the diff or list touched files.

## Pull Request Template

Pull request descriptions are checked automatically when they are opened, edited, reopened, or marked ready for
review. Keep the `## Summary`, `## Motivation`, `## Type of Change`, `## Testing`, and `## Checklist` headings and the
Checklist wording from [`.github/PULL_REQUEST_TEMPLATE.md`](.github/PULL_REQUEST_TEMPLATE.md). The remaining sections
are context only: fill them in, leave them empty, or delete them. In Type of Change, keep only the lines that apply.

Draft pull requests may leave checkboxes incomplete. Before marking a pull request ready for review, check exactly one
of Bug fix, New feature, Refactoring, Build / packaging, or Documentation only (add Breaking change alongside it when it
applies), and check every item in the Checklist section. A pull request that is missing required template structure is
commented on and converted back to a draft; add the missing content and mark it ready for review to run the check
again. The check never closes a pull request.

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

Keep the `common` namespace narrow, for actions shared across the UI. Settings labels live under
`settings.widgets.settings.<config_key>.label` and `settings.widgets.settings.<config_key>.description`.

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
    modem/          ModemManager cellular integration
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
  fonts/            Bundled Noctalia Tabler and UI fonts
  sounds/           Notification and UI sounds
  templates/        Built-in theme templates
  translations/     Exported translation catalogs
protocols/          Vendored Wayland protocol XML files
tests/              Unit tests and config validation fixtures
tools/              Developer and translation helper scripts
nix/                Nix package, module, and dev shell definitions
third_party/
  wuffs/          Raster image decoding (vendored)
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

### Crash output

When reporting a crash, include the complete terminal output from the process if it is available. Do not paste only
`Segmentation fault`; that line does not contain a useful stack trace or the error context.

### ASan crash reports

AddressSanitizer (ASan) can find memory errors that a normal build reports only as a crash. It requires a temporary
source build; it does not replace the Noctalia package you already use.

Install the source-build dependencies for your distribution using the commands in
[BUILDING.md](BUILDING.md#dependencies), then clone the repository:

```sh
git clone https://github.com/noctalia-dev/noctalia.git
cd noctalia
```

Stop the Noctalia instance started by your compositor, then configure and build ASan:

```sh
just configure asan && just build asan
```

Start the ASan binary in the foreground and save its output:

```sh
ASAN_OPTIONS=log_path=/tmp/noctalia-asan ./build-asan/noctalia 2>&1 | tee noctalia-asan-terminal.log
```

Reproduce the crash once. If Noctalia does not exit, press `Ctrl+C`. Attach both `noctalia-asan-terminal.log` and every
`/tmp/noctalia-asan.*` file to the GitHub issue. The files in `/tmp` preserve the ASan report if the crash takes down
the terminal. If the build or startup fails, attach that complete output instead.
