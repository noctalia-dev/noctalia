# Contributing

This file collects contributor-facing details for Noctalia: design goals, stack notes, code style, contribution rules, testing, commit conventions, source layout, and debugging helpers.

For dependencies and normal build commands, start with [BUILDING.md](BUILDING.md). For the architecture and the invariants reviewers enforce (UI phases, layer boundaries, rendering, surfaces, config, plugins), read [ARCHITECTURE.md](ARCHITECTURE.md).

Before contributing, read our [ethos](https://noctalia.dev/ethos) to understand the values and philosophy guiding the project.

## Design Principles

- Direct Wayland + OpenGL ES only -- no toolkit overhead
- Minimal scene graph, domain-specific to shell UI
- Packaging should work across all major Linux distros: Arch, NixOS, Fedora, Gentoo, Debian, Void, openSUSE

## Stack

Direct project dependencies are listed below; transitive dependencies are owned by their providing system packages.

| Layer                      | Library                                                                                                                                                             |
| -------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Wayland core               | `libwayland-client`, `wayland-scanner`, `wayland-protocols`                                                                                                         |
| Surfaces                   | `xdg-shell`, `zwlr-layer-shell-v1`                                                                                                                                  |
| Multi-monitor              | `zxdg-output-unstable-v1`                                                                                                                                           |
| Active window metadata     | `zwlr-foreign-toplevel-management-unstable-v1`                                                                                                                      |
| Workspaces                 | `ext-workspace-v1`, `dwl-ipc-unstable-v2`                                                                                                                           |
| Clipboard                  | `ext-data-control-v1`, `wlr-data-control-unstable-v1`                                                                                                               |
| Activation                 | `xdg-activation-v1`                                                                                                                                                 |
| Lockscreen                 | `ext-session-lock-v1`                                                                                                                                               |
| Idle                       | `ext-idle-notify-v1`, `idle-inhibit-unstable-v1`                                                                                                                    |
| Cursor                     | `wp-cursor-shape-v1`                                                                                                                                                |
| Keyboard                   | `xkbcommon`                                                                                                                                                         |
| Rendering                  | `EGL`, `OpenGL ES 2.0+`, `wayland-egl` (`libepoxy` fallback)                                                                                                        |
| Dynamic loading            | `libdl` when provided separately by the platform                                                                                                                    |
| Text                       | `cairo`, `cairo-ft`, `pango`, `pangocairo`, `pangoft2`, `harfbuzz`, `freetype2`, `fontconfig`                                                                       |
| Images                     | `Wuffs` (vendored), `stb_image_resize2`, `stb_image_write`, `libwebp`, `libjxl`, `libjxl_threads`, `librsvg`, `libheif` (optional, loaded at runtime with `dlopen`) |
| IPC and service runtime    | `sdbus-c++`, `glib-2.0`, `gobject-2.0`, `gio-2.0`                                                                                                                   |
| Audio                      | `libpipewire-0.3`, `wireplumber-0.5`, `libsndfile`                                                                                                                  |
| Authentication             | `PAM`, `polkit-agent-1`, `polkit-gobject-1`                                                                                                                         |
| Credentials and encryption | `libsecret-1`, `libsodium`                                                                                                                                          |
| HTTP                       | `libcurl`                                                                                                                                                           |
| XML                        | `libxml2`                                                                                                                                                           |
| Calendar data              | `libical`                                                                                                                                                           |
| Config                     | `tomlplusplus`                                                                                                                                                      |
| JSON                       | `nlohmann/json`                                                                                                                                                     |
| Markdown                   | `md4c`                                                                                                                                                              |
| Fuzzy matching             | `fzy` (vendored)                                                                                                                                                    |
| Math expressions           | `libqalculate`                                                                                                                                                      |
| Scripting                  | `Luau` (vendored)                                                                                                                                                   |
| Theme generation           | Material Color Utilities (vendored)                                                                                                                                 |
| Memory allocation          | `jemalloc` (optional)                                                                                                                                               |

## Code Style

Run `just format` before committing and `just format-check` to check. Both run a code half (`format-code`, `format-code-check`) and a docs half (`format-docs`, `format-docs-check`), which you can also run on their own.

Code formatting accepts [clang-format](https://clang.llvm.org/docs/ClangFormat.html) 23 or newer and covers `lib/`, `src/`, and `tests/`. The code recipes reject formatter versions older than 23 and print the version in use.

CI runs `just format-code-check` and `just lint` on Arch with clang-format and clang-tidy 23, and fails if the installed major differs. Newer versions may produce different formatting or diagnostics, so a local check with a newer toolchain does not guarantee CI will pass. Use version 23 to reproduce or resolve CI formatting and lint failures.

For CI parity, install `clang` on Arch (currently 23). If your distribution installs Clang 23 outside the default PATH, prepend its binary directory when running either recipe, for example `PATH=/usr/lib/llvm23/bin:$PATH just format-code-check`. Keep editor formatting on the same version as your command-line formatter.

Markdown and MDX files are formatted by `tools/remark/format.mjs`, which parses them with the same remark plugins as the docs site (MDX, GFM, directives, custom heading ids, frontmatter) plus math, so it reads every file as the site and GitHub render it. Its dependencies are pinned in `tools/remark/package-lock.json`; the docs recipes need Node.js and npm and install them into `tools/remark/node_modules` on first use. Do not hard-wrap prose: the formatter writes each paragraph, list item, and table cell on one line, keeping GitHub alert markers (`> [!NOTE]`) on their own line. It refuses to write a result whose parsed document differs from the input beyond whitespace. CI runs `just format-docs-check`.

For editor integration, `just configure` creates a root `compile_commands.json` symlink to the selected Meson build directory. Run `just configure`, `just configure release`, or `just configure asan` for the build mode you want clangd to use.

The repo also includes `lefthook.yml`. Run `lefthook install` to install the pre-commit hook; it runs `just format-code` when C++ files are staged and `just format-docs` when Markdown or MDX files are staged, then refreshes the git index for tracked formatting changes.

### Lint

Code must pass clang-tidy with warnings treated as errors. `just lint` checks the whole tree; while iterating, lint only the `.cpp` files you touched:

```sh
clang-tidy -p build-debug --warnings-as-errors='*' <touched .cpp files...>
```

Headers are linted through the `.cpp` files that include them. Fix every finding before opening a pull request.

sdbus-c++ async reply handlers take the error as `std::optional<sdbus::Error>` by value. sdbus-c++ releases before 2.3 (shipped by Fedora 44, Debian 13, and Ubuntu 26.04) do not recognize a `const&` error parameter, so the code fails to compile there even though it builds on Arch. Suppress the resulting clang-tidy finding on that lambda with `// NOLINT(performance-unnecessary-value-param)`. The `Build (Fedora 44)` CI job guards this.

### Naming Conventions

|                        | Convention           | Example              |
| ---------------------- | -------------------- | -------------------- |
| Files                  | snake_case           | `widget_factory.cpp` |
| Directories            | snake_case           | `shell/bar/widgets/` |
| Types / Classes        | PascalCase           | `WidgetFactory`      |
| Functions / Methods    | camelCase            | `createWidget()`     |
| Variables / Parameters | camelCase            | `busName`            |
| Private members        | m_camelCase          | `m_changeCallback`   |
| Enumerators            | PascalCase           | `TextAlign::Center`  |
| Macros                 | SCREAMING_SNAKE_CASE | `MAX_SIZE`           |

clang-format inserts braces around every control statement body and clang-tidy enforces the naming table for enumerators and private members.

Scoped enums (`enum class`) use an explicit `std::uint8_t` underlying type where it makes sense.

Getters are the noun, without a `get` prefix: `width()`, `selected()`, `textureManager()`.

D-Bus wire-protocol string literals, such as `player["bus_name"]`, stay snake_case because they are wire names, not C++ identifiers.

### Source Conventions

- `lib/` and `src/` are the include roots; project includes are relative to them (`#include "ui/controls/button.h"`, `#include "app/application.h"`).
- `lib/` (`core`, `i18n`, `render`, `ui`, `util`, `wayland`) is the self-contained `noctalia-ui` library (`noctalia_ui_dep`) plus the opt-in codec pack (`noctalia_ui_codecs_dep`, `lib/render/codecs/`). It compiles with only `lib/` and its own generated protocol headers on the include path, so it cannot include shell code from `src/`, and nothing in `lib/` references files outside it. Code that only the shell uses belongs in `src/`, even when it looks generic. See ARCHITECTURE.md for what the application supplies at startup.
- `lib/meson.build` lists every library source, finds the library's dependencies, and generates the Wayland protocols the library binds from `lib/protocols/` and wayland-protocols; the root `meson.build` lists every shell source, generates the shell-only protocols, and runs `subdir('lib')`. There are no other per-directory build files.
- Library tests link `noctalia_ui_dep` (or `noctalia_ui_codecs_dep`) and `tests/test_identity.cpp`, which sets the shell's identity before `main()`. Tests of shell code link `noctalia_core_dep`, which includes `tests/test_shell_library.cpp`: it calls `configureSharedLibrary()` before `main()`, as the shell does.
- Headers and sources are co-located; there is no separate `include/` tree.
- Use `[[nodiscard]]` only where ignoring the result is likely a bug: a call with side effects that reports failure or hands back something the caller must own, consume or keep (a handle, token or id), and `&&` builders. Mark result and status types with `struct [[nodiscard]] Name` instead of each function that returns them. Getters and pure functions go without it.

### Standalone UI library consumers

A consumer copies `lib/` and runs `subdir('lib')` to obtain `noctalia_ui_dep`. The core does not configure or build the codec pack, probe its dependencies, or require `lib/render/codecs/` or `lib/third_party/`. Install an application-owned `ImageDecoder` when supplying your own image formats.

To enable the bundled codec pack, set `noctalia_ui_build_codecs = true` before `subdir('lib')` and link `noctalia_ui_codecs_dep`. This also defines `noctalia_ui_codecs_lib`. `noctalia_ui_system_wuffs = true` selects system Wuffs headers when the codec pack is enabled. The Noctalia shell explicitly enables the pack; its existing image support and dependencies are unchanged.

Applications with their own asset layout call `paths::setAssetRoots({.application = applicationAssets, .library = libraryAssets})` from `core/files/resource_paths.h` once at startup, before any asset lookup or workers. The application chooses its canonical environment/install paths and validates its own required files. The library validates both directories and requires regular `fonts/noctalia-tabler.ttf` and `fonts/tabler.json` files in the library root. The roots may be the same installed bundle or separate application and `lib/assets` directories. Relative roots are anchored to the working directory at configuration time. Invalid roots throw; explicit roots never enter discovery or require a translation catalog. Reconfiguration and configuration after the first lookup throw to preserve borrowed path references.

Without explicit roots, the shell keeps its existing application-bundle discovery through its environment override, executable location and install directory, identified by `translations/en.json`. Library discovery checks the application bundle and build-tree `lib/assets` for both the font and metadata; missing library assets are an error.

### Comments

Comments explain what the current code does and any non-obvious constraint a reader needs. Do not narrate history, rejected alternatives, or "why we don't do X"; git holds that. If a comment is longer than the code it describes, cut it down.

## Contribution Rules

### One change per pull request

Keep each pull request to a single feature, fix, or refactor. Unrelated changes, including drive-by cleanups and formatting of untouched code, go in separate pull requests so each can be reviewed and reverted on its own.

### One canonical name, no fallbacks

Every config key, IPC name, path, and identifier has exactly one canonical name. Do not add aliases, "try name X then name Y" resolution, backwards-compatibility shims, or silent default chains that hide misconfiguration. When input is wrong or missing, fail or report it loudly instead of papering over it. A maintainer may explicitly request a migration path; do not add one unprompted.

This applies to identity and configuration, not runtime resilience. Graceful degradation of an operation, such as serving a cached last-known-good value when a network or IO call fails, is fine.

### User documentation

End-user documentation lives in [`docs/user/`](docs/user/) (MDX), with images under `docs/assets/`. Any change that adds, removes, or changes user-facing configuration, or behavior users need to know about to use or configure the shell, updates the relevant page in the same commit as the code. `docs/user/` is user documentation, not a feature changelog: don't document purely cosmetic or self-explanatory UI additions.

The documentation site is generated from these files by `tools/sync-docs.sh`, which copies `docs/user/**/*.mdx` into the docs site checkout and deletes pages that no longer exist here. Never edit the generated copies; the next sync overwrites them.

The same script archives each `vX.Y.Z` release tag once, from the tag's own `docs/user/`, `docs/assets/`, and `docs/plugin-api.json`, under `/noctalia/<tag>/`, and writes the version list the site's switcher reads. It needs `jq` and a checkout with the release tags fetched. Archives are never rewritten afterwards; run it with `FORCE_SYNC=1` to rebuild them after changing the archiving itself.

### Vendored code

Do not patch vendored code under `third_party/` or `lib/third_party/` for Noctalia behavior; local edits are lost when the dependency is refreshed. Put compatibility code in Noctalia-owned sources under `lib/` or `src/`, or fix the issue upstream and update the vendored snapshot from upstream.

### Palette colors

Fills, tints, and borders use a palette role: `surface_container_low` to `surface_container_highest` for raised backgrounds, the `*_container` roles for tinted fills, and `outline_variant` for borders. Do not blend a role with a hardcoded alpha (`colorSpecFromRole(ColorRole::Primary, 0.16F)`) to get a color the palette does not have; if no role fits, propose a new one. Alpha is for real transparency only: shadows, scrims, overlays, disabled content, and hover or pressed state layers, ideally through a named `Style` constant. `just role-alpha-check` (run in CI) counts role-plus-literal-alpha uses and fails when the count goes up; after removing some, run `just role-alpha-update` to lower the baseline.

## Testing

Unit tests are not required for every change. Choose the verification that exercises the changed behavior most directly, and describe it in the pull request's Testing section.

Add or modify a permanent unit test only when all of the following hold:

- It protects an observable, deterministic contract.
- A plausible future regression would have meaningful impact.
- Existing tests do not already cover the contract.
- The test would fail for a plausible broken implementation.
- The test is cheaper and more reliable than exercising the behavior in the running shell.

Unit tests are most valuable for configuration parsing and migration, persistent data, security boundaries, parsers and decoders, non-trivial state machines, and deterministic protocol transformations.

Do not add tests for trivial forwarding, getters, constants, duplicated implementation logic, implementation details, or visual behavior modelled through mocks. Rendering, layout, animation, Wayland lifecycle, and compositor integration changes are verified in the real shell; do not synthesize brittle pointer-event or mock-based tests to avoid a visual check.

For bug fixes, reproduce the bug and confirm the reproduction no longer fails. Add a regression test only when it provides durable coverage beyond that check.

Write test checks with `TEST_CHECK` from `tests/test_check.h`, never `assert`. `assert` compiles away under `NDEBUG`, so a release build would pass a broken test silently; `TEST_CHECK` prints the failed expression with its file and line and exits non-zero. The header refuses to compile with `NDEBUG`, so test executables set `override_options: ['b_ndebug=false']` in `meson.build`.

Compare parsed floating-point values with an appropriate tolerance, not exact equality to a decimal literal. For double-precision fixtures near 1, use `std::abs(actual - expected) <= std::numeric_limits<double>::epsilon()`, as in the plugin manifest tests. This also accommodates excess precision on 32-bit x87 builds.

## Commit Messages

Follow the Conventional Commits style used in the history: `type(scope): summary`, or `type: summary` when no scope helps. Use lowercase types such as `feat`, `fix`, `refactor`, `chore`, `test`, `i18n`, `ui`, or `config`, and a focused subsystem scope when useful, for example `refactor(bar): use widget option structs`. Keep the summary concise, imperative, and specific.

Default to the summary line alone. Add a body only when it carries information a reader would otherwise miss: a non-obvious root cause, a constraint that forced the approach, a behavior change users will notice, or a `Fixes #N` reference. Do not restate the diff or list touched files.

## Pull Request Template

Pull request descriptions are checked automatically when they are opened, edited, reopened, or marked ready for review. Keep the `## Summary`, `## Motivation`, `## Type of Change`, `## Testing`, and `## Checklist` headings and the Checklist wording from [`.github/PULL_REQUEST_TEMPLATE.md`](.github/PULL_REQUEST_TEMPLATE.md). The remaining sections are context only: fill them in, leave them empty, or delete them. In Type of Change, keep only the lines that apply.

Draft pull requests may leave checkboxes incomplete. Before marking a pull request ready for review, check exactly one of Bug fix, New feature, Refactoring, Build / packaging, or Documentation only (add Breaking change alongside it when it applies), and check every item in the Checklist section. A pull request that is missing required template structure is commented on and converted back to a draft; add the missing content and mark it ready for review to run the check again. The check never closes a pull request.

## Translations

Noctalia translations are managed through [Noctalia Translate](https://i18n.noctalia.dev/projects/noctalia). The JSON files in `assets/translations/` are exported from that workflow, with `assets/translations/en.json` acting as the source catalog for new strings.

When a code, UI, settings, or documentation change needs a new user-facing string, add or update the English string in `assets/translations/en.json` only. Do not machine translate strings, copy English into other locales, or include broad updates to non-English translation files in a normal feature or bug-fix PR. The translation team handles those locale updates through the translation app.

Only edit non-English translation files when the PR is explicitly about translation tooling, an import/export sync, or a maintainer has asked for that specific locale change.

Keep the `common` namespace narrow, for actions shared across the UI. Settings labels live under `settings.widgets.settings.<config_key>.label` and `settings.widgets.settings.<config_key>.description`.

After adding or renaming translation keys, run:

```sh
python3 tools/i18n-check.py
```

## Project Layout

```text
lib/                Shared UI library (noctalia-ui); self-contained, built without src/ on its include path
  meson.build       Library targets (noctalia-ui, opt-in noctalia-ui-codecs), dependencies, and the Wayland protocols they bind
  assets/fonts/     Glyph font and Tabler metadata the library loads
  protocols/        Non-wayland-protocols XMLs the library binds (text-input-v3, cursor-shape)
  third_party/
    wuffs/          Raster image decoding for the codec pack (vendored)
  core/             Application identity, UI strings hook, logging, timers, resource paths, input chords
  i18n/             Translation catalog and language tag handling
  render/
    animation/      Animation manager, easing, motion settings
    backend/        GLES render backend, framebuffers, texture manager
    codecs/         Opt-in codec pack: raster/SVG/HEIF decoders, image encoders, thumbnails
    core/           Render data types, image decoder interface, texture caches
    programs/       GLES shader programs
    scene/          Scene graph nodes and pointer input dispatch
    text/           Cairo/Pango text and glyph rendering
  ui/
    controls/       Reusable controls (Button, Input, Label, Select, Slider, Box, ...)
  util/             Generic helpers
  wayland/          Wayland connection, registry extensions, seats, xdg toplevel/popup/sub surfaces, text input
src/
  main.cpp          Entry point
  app/              Application bootstrap, build info, main loop, poll sources
  auth/             PAM and fingerprint authentication
  calendar/         CalDAV, Google Calendar, iCalendar parsing, polling
  capture/          Screenshots, screencopy capture, region overlay
  compositors/      Compositor detection, runtime adapters, workspace/output/keyboard backends
  config/           Configuration schema, validation, hot reload, state store, overrides
    schema/         Typed config schema engine
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
  idle/             Idle manager, inhibitor, grace overlay
  ipc/              IPC client/service, CLI command parsing, plugin pixel stream companion protocol
  launcher/         Launcher providers (apps, emoji, calculator, sessions, windows, plugins), fuzzy matching
  net/              HTTP client, URI parsing, URL opening
  notification/     Notification model, manager, filtering, history
  pipewire/         PipeWire audio service, sound playback, spectrum analyzer
  scripting/        Luau plugin runtime, manifests, registry, source management, bindings
  shell/
    backdrop/       Backdrop layer surfaces
    bar/            Bar surface, instance, widget base/factory
      widgets/      Bar widget implementations
    clipboard/      Clipboard history panel and paste helpers
    control_center/ Control center panel, tabs, shortcut registry
    controls/       Shell-owned controls: markdown view, search picker, color and glyph pickers
    desktop/        Desktop widget host, factory, layout, editor support
      widgets/      Desktop widget implementations
    dialogs/        Dialog popup hosts; file, color, glyph, and search picker dialogs
    dock/           Dock surface, model, pinned apps, context menu
    greeter/        Greeter appearance sync
    launcher/       Launcher panel
    lockscreen/     Session lock surfaces and lockscreen widgets
    notification/   Notification toasts
    osd/            On-screen display overlays
    overview/       Overview capture helpers
    panel/          Panel base, manager, attached-panel context
    polkit/         Polkit prompt panel
    render/         Shell-only custom draw nodes, shader programs, and render styles
    screen_corners/ Screen corner overlays
    session/        Session panel and action runners
    settings/       Settings window, setting controls, registries, popups
    setup_wizard/   First-run setup wizard
    surface/        Shared shell surface geometry and shadow helpers
    switcher/       Window switcher UI
    tooltip/        Tooltip manager
    tray/           Tray drawer and D-Bus menu UI
    visuals/        Audio visualizer and graph controls
    wallpaper/      Wallpaper surfaces, paths, picker panel
    wayland/        Shell-only Wayland extensions: layer shell and layer surfaces, blur, toplevels, workspaces, clipboard, capture, idle, output heads, focus grab
    widgets_editor/ Background widget editor
  system/           Desktop entries, directory scanning, brightness, weather, location, system monitor, hardware services
  theme/            Palette generation, templates, theme service, template application
  time/             Time service, polling, and time formatting
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

When reporting a crash, include the complete terminal output from the process if it is available. Do not paste only `Segmentation fault`; that line does not contain a useful stack trace or the error context.

### ASan crash reports

AddressSanitizer (ASan) can find memory errors that a normal build reports only as a crash. It requires a temporary source build; it does not replace the Noctalia package you already use.

Install the source-build dependencies for your distribution using the commands in [BUILDING.md](BUILDING.md#dependencies), then clone the repository:

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

Reproduce the crash once. If Noctalia does not exit, press `Ctrl+C`. Attach both `noctalia-asan-terminal.log` and every `/tmp/noctalia-asan.*` file to the GitHub issue. The files in `/tmp` preserve the ASan report if the crash takes down the terminal. If the build or startup fails, attach that complete output instead.
