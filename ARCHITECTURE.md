# Architecture

This file describes the architecture inherited from Noctalia and the technical invariants that still apply to Nythera. It should be updated as Nythera's implementation diverges. Application-owned identifiers and code references below use Nythera. Upstream-hosted services keep their original endpoints. For code style, testing, commit conventions, and the project layout, see [CONTRIBUTING.md](CONTRIBUTING.md).

## Build Targets

The monorepo builds reusable infrastructure as `nythera_lib`. The Shell executable adds `nythera_shell`. Files adds `nythera_files` and `nythera_file_operation_engine`. These are internal build targets. Shared surface and input protocols and Shell protocol bindings build in separate targets. Shell opts into the extra registry bindings through its tagged `WaylandConnection` constructor. Configure Meson with `-Dshell=false` for a Files-only build and install. The combined test suite uses the default configuration.

## Event Loop

The main loop is poll based (`MainLoop` plus the `PollSource` interface). Every event source (Wayland, D-Bus, inotify, timers) exposes file descriptors that are polled together.

A new service implements `PollSource`, is added as an `Application` member, and is returned from `buildPollSources()`. Each application supplies its own stop predicate, poll-source provider and shutdown callback. Shell owns bar shutdown.

## UI Phases

Each surface runs explicit phases per frame:

1. `PrepareFrame`: tick animations, frame callbacks
2. `Update`: data and state synchronization
3. `Layout`: geometry and subtree rebuilds
4. `Render`: paint only

Rules:

- Never run layout or mutate the scene graph during Render.
- Structural rebuilds belong in Layout, state sync in Update; visual-only changes use `requestRedraw()`.
- Rebuild helpers (`rebuild...()`, `buildScene()`) are `private` and assert they are not running in the Render phase.
- Setters and callbacks schedule work instead of rebuilding directly:
  - `requestUpdate()` for data or state changes
  - `requestLayout()` for geometry or structure changes
  - `requestRedraw()` for paint-only changes
  - `requestFrameTick()` for data-fed animations that must apply data before painting
- Frame callbacks and configure handlers queue renders; they never render synchronously from Wayland dispatch.
- Scene-root swaps dispatch leave on the old tree and replay pointer enter on the new tree.

Guarded entry points: override the hook, not the public wrapper.

| Public entry                                       | Override                      |
| -------------------------------------------------- | ----------------------------- |
| `Node::layout()`                                   | `doLayout()`                  |
| `Node::measure()` / `Node::arrange()`              | `doMeasure()` / `doArrange()` |
| `Panel`, `Tab`, `Widget` `.layout()` / `.update()` | `doLayout()` / `doUpdate()`   |

Inside a hook, call the parent hook (for example `Flex::doLayout()`), not the public wrapper.

## Layer Boundaries

**Shell and widget code** (`src/shell/`) uses only controls from `src/ui/controls/` (`Label`, `Button`, `Glyph`, `Box`, `Separator`, ...). It never creates raw scene nodes such as `TextNode`, `RectNode`, or `IconNode`. `InputArea` is the only exception. Use `Box` for backgrounds, `Separator` for dividers, and `Label` for text.

**Controls** (`src/ui/controls/`) own the raw nodes.

- `Label::measure(Renderer&)` must be called after setting text and font, before positioning.
- Use `VirtualGridView` for fixed-size cell grids and `VirtualListView` for variable-height lists.
- A control composes its child nodes in its constructor, implements layout, and animates through `AnimationManager`.

**Composition**: prefer the declarative `ui::` builders (`ui::row`, `ui::column`, `ui::label`, `ui::button`, ...) wherever a builder exists. Use direct construction and setters only for properties or lifecycle work the builders cannot express. Primitive controls may construct their own internal nodes directly; they do not depend on the aggregate builder factory for that.

## Filesystem Operations

Nythera Files runs local copy, duplicate, move, rename, create and permanent delete through `file_operation_engine`. The engine runs synchronously on the caller's worker thread and owns filesystem traversal, conflict decisions, staging, recovery and pause checkpoints. It uses GIO and `fs_compat` for filesystem access and reports progress through callbacks.

Copy and Duplicate use the destination's ownership rules. New directories use creation modes and regular file copies retain ordinary access bits with set-ID bits cleared. Move and Rename retain the inode when possible. The staged move fallback restores and verifies the original owner, group and mode before removing the source.

`FileOperationManager` owns GTask dispatch, progress presentation, session D-Bus jobs, mount protection, history and shutdown. Wastebin and Undo/Redo reuse filesystem primitives from the engine. For Administrator Mode, the manager submits protocol v9 operations through the originating tab’s session client and consumes the results on its worker. The helper runs the same engine. The GUI uses engine-provided conflict metadata and the existing progress and job presentation.

Protected file export, explicit write-back, checksums and recursive folder sizes also use the originating tab’s session and the shared engine. Foreground reads and background thumbnail exports can proceed independently of filesystem mutations. Temporary copies are owned by the signed-in user, and external applications run under that user’s credentials.

## Scene Graph and Input

`Node` is the scene graph base class. `InputArea` is a `Node` subclass for pointer hit testing, and `InputDispatcher` routes pointer events through the tree. `InputArea::setAcceptedButtons()` takes masks built with `InputArea::buttonMask(...)`, not raw `BTN_*` codes.

## Rendering

- `RenderContext` owns scene traversal and delegates drawing to a `RenderBackend`. Construct backends with `createDefaultRenderBackend()`.
- Texture allocation goes through `TextureManager`, constructed with `createDefaultTextureManager()`.
- Shell and widget code works with typed render concepts (`TextureId`, `TextureHandle`, `RenderFramebuffer`, `RenderSurfaceTarget`, nodes, draw primitives). Raw `GLuint` or EGL handles stay inside the render infrastructure.
- `RenderContext` does not own shader programs or include `render/programs/*`. Style and data structs live in neutral headers under `render/core/`.
- A new shader follows the same pattern: a GLES program in `src/render/programs/`, neutral data under `render/core/`, a typed `RenderBackend` draw method, and ownership by `GlesRenderBackend`.
- Text is rasterized with Pango and Cairo, uploaded through `TextureManager`, and drawn with `RenderBackend::drawGlyph()`.
- Async images are consumer scoped through `Image::setSourceFileAsync()` and `AsyncTextureCache::subscribeReady()`; there is no fan-out from `Application`.

**Rectangle shapes**: the rect shader is generic. Use per-corner `CornerShape::Convex` or `CornerShape::Concave` with `Radii` and `RectInsets logicalInset`. Panel and bar policy stays in shell code, not in shader enum names. Screen-corner overlays use the dedicated `ScreenCornerProgram` and `ScreenCornerNode` path.

### Native background haze

- Native surface hosts own haze profile IDs and per-instance pattern/time state. `SurfaceHaze` uses one shared TimerManager timer and normal Wayland frame readiness; static, disabled, closed and suspended hosts do not request haze frames. Haze redraws bypass unrelated content frame-tick callbacks.
- Existing background materials opt in through Box/Flex or screen-frame style flags. RenderContext supplies neutral `SurfaceHazeStyle` data; GLES rectangle and contour programs reuse one shader function. Foreground alpha and native shape coverage remain independent.
- Bars and islands are excluded. A bar/screen-wrap contour that includes an attached panel may shade only the explicit panel-body bounds under that panel's profile. Transparent input/capture planes do not gain synthetic backgrounds.
- Configuration retains `shell.panel.haze_*` paths and an independent `haze_surfaces` map; the Settings UI owns a dedicated Haze section. See [Nebula panel background](docs/nebula-background-poc.md) for coverage and lifecycle details.

### Fractional Scaling

- With `wp_fractional_scale_v1` and `wp_viewporter`, keep `set_buffer_scale(1)`, render at the preferred fractional scale, and set `wp_viewport.set_destination()` to the logical size.
- Set the `RenderContext` on a surface before its callbacks or `initialize()`.
- Configure-time builds call `makeCurrent()` and `syncContentScale()` before any layout, measurement, or texture work.
- Cross-surface callbacks sync the target surface's render scale.
- A fractional-scale change affects layout; text caches key on render scale and font generation.

### Text Vertical Centering

Vertical text centering lives in the `Label` engine, never in widgets. Widgets box-center the label and do no font math.

- In Text mode, `Label` places the baseline so the cap band is centered: `baselineOffset = height/2 + capHeight/2`.
- `capHeight` is the measured baseline-to-cap-top of `H` from `Renderer::measureFont().capHeight` for the label's own font family, a stable per-font value. It is not the OS/2 declared cap height, and not the label's own per-string ink.
- Do not center on the line box (`(ascent - descent)/2`); it places caps too high on fonts with a large descent.
- Do not pre-round text positions. `CairoTextRenderer::draw()` snaps the final glyph quad to the buffer pixel grid, so rounding earlier double-rounds and causes 1px drift. Round box and rect positions; leave text positions fractional.
- A label that is a single Private Use Area codepoint (`StringUtils::isSinglePrivateUseGlyph`) is an icon glyph. It is sized to its ink on both axes and centered on that ink instead of the cap band.
- `Label` ceils its box width, so the box is never narrower than the text.
- Line breaking is decided once, on the measure pass. The arrange pass reuses that wrap budget; an arrange width is a box size, not a new wrap request.
- When debugging alignment, instrument the real renderer (env-gated logging) instead of modelling it.

## Surfaces

### Panels

- Every panel (attached, detached, floating, centered) owns its own `nythera-panel` layer surface and draws its own background, shadow, blur, and reveal animation.
- "Attached" is a placement mode only: the panel's own surface is anchored to the bar's edge at the bar's layer, with a clipped reveal. It is not hosted in the bar surface.
- Workspace-map previews reuse `ToplevelThumbnailCapture::capture()` through one shared, serialized capture slot, keyed by compositor window ID. The initial visible snapshot sweep has no idle gap; ongoing previews share at most four captures per second, with adaptive backoff, rather than owning independent live sessions. Pause stops ongoing refreshes but still fills missing visible snapshots; dragging and native-window suspension gate capture work. Closing either host cancels capture and scheduling and drops the bounded frame cache.
- The expanded workspace map uses a native `ToplevelSurface`, like Settings, and hosts the same map content as the compact panel. Its input, move/resize requests and lifetime belong to the native window host; the compact map remains in `PanelManager`.
- A panel can opt into corner resizing through `PanelManager::beginPanelResize()`. The host owns surface resizing, output bounds, input regions and attached geometry; `Panel::onResizeFinished()` lets the owning panel remember its final size in UI state.
- Attached and single-bar detached placement is computed before commit against a specific bar on a specific output, so a panel needs a concrete output before commit. A NULL layer-shell output would mis-anchor it.

### Bars, Dock, and Layer Order

- `Bar` creates per-display instances through `WidgetFactory`. The `Widget` base follows a `create()`, `layout()`, `update()` lifecycle. A new bar widget subclasses `Widget` and is registered in `WidgetFactory`.
- Bar capsule cross-size derives from bar thickness, not widget content. `WidgetBarCapsuleSpec::enabled` controls outer capsule drawing; its size and style fields may still be populated for internal reuse.
- Bars are created before the dock. On hot reload geometry changes, recreate dock surfaces after bars.

### Shadows and Blur

- Shadow style is global (`ShellConfig::ShadowConfig`, `[shell.shadow]`). Components only expose `shadow = true|false`. Shared logic lives in `src/shell/surface/shadow.*`.
- Bar, dock, and notification surfaces always publish blur regions when the compositor supports them. There are no per-component blur toggles.

### Notification Toasts

Toast slots are stable for a toast's lifetime; surviving toasts keep their slot and new toasts fill freed slots. If every slot is hovered, new toasts queue off screen.

### Desktop Widgets

Desktop widgets live under `src/shell/desktop/` and do not use the bar `Widget` base. Use `wantsSecondTicks()` for clocks; use `needsFrameTick()` and `onFrameTick()` only for continuous animation.

## Compositor Integration

- `WaylandConnection` is protocol transport only, with no compositor-specific branches. Compositor behavior goes through `CompositorPlatform`.
- `WaylandConnection::Purpose::ApplicationClient` binds the ordinary xdg application protocols needed by standalone native apps and skips shell-only layer-shell and service globals. Native clipboard and drag-and-drop use the core data-device protocol through `NativeDataTransfer`; the shell data-control history service remains separate. Each MIME representation has its own bytes, and transfer pipes participate in the application poll loop. Shell selects its registry bindings explicitly. Screencopy connections retain their bounded sibling connection path.
- Prefer passing `CompositorPlatform&` over passing both `WaylandConnection&` and `CompositorPlatform&`.
- `CompositorPlatform::outputWorkArea()` optionally supplies global logical bounds excluding reserved bars. Compositor-specific queries and parsing stay in the integration backend; callers fall back to full output bounds when metadata is unavailable.
- Detect compositors with `compositors::detect()`, `isHyprland()`, `isNiri()`, `isSway()`, and `isMango()` from `compositor_detect.h`; do not sniff environment variables again.
- Logout goes through `uwsm stop` when UWSM manages the compositor, otherwise compositor-native logout commands. Niri goes through `NiriRuntime` IPC, not the CLI.

## Configuration and State

- Schema structs and enums live in `src/config/config_types.*`. `ConfigService` owns lifecycle and hot reload. The shared loader reads and merges settings and overrides. Shell owns override mutation in `src/shell/config/config_mutations.cpp` and supplies its schema validation hooks.
- Files reads the shared settings for appearance and shortcuts, including applying migrations in memory. It writes its own preferences through the state APIs.
- The settings GUI writes config overrides to `settings.toml`. Internal app and UI state lives in `state.toml` under owner tables (for example `[wallpaper_panel]`), accessed through `StateStore` or the `ConfigService` state APIs. Do not add ad hoc state files or hide internal state under config sections.
- Settings controls: `SearchPickerSetting` for large catalogs, `SelectSetting` for short dropdowns.
- Settings entries carry a `group`. Rendering routes through `coalesceByGroupKey()` (`settings_registry.h`), so each group renders once in first-appearance order. Still declare related entries together.

## IPC and D-Bus

- Register IPC handlers in the module that owns the behavior. Scripted-widget IPC is bar owned and target explicit.
- D-Bus services release their well-known names on disable or destroy.
- Daemon mode forks before IPC, GLib, D-Bus, or render initialization, then execs.
- The first completed async D-Bus state snapshot is a baseline, not a user-visible transition.
- StatusNotifier and DBusMenu: tray apps may be non-standard, so method and property failures are non-fatal. Menu layout loading is async and retained; popup creation never blocks on synchronous D-Bus calls.

## Services

- **Idle**: `IdleManager` runs `ext_idle_notifier_v1` behaviors. Each `[idle.behavior.*]` has a `command` and an optional `resume_command`. `nythera:dpms-off` gets an automatic `nythera:dpms-on` on resume. `pre_action_fade_seconds` drives `IdleGraceOverlay` on overlay layer surfaces.
- **PipeWire**: access SPA pod arrays with the stable `SPA_POD_ARRAY_*` macros from `spa/pod/pod.h`, not `spa_pod_get_array_full()` (added in PipeWire 1.6, absent in 1.4). Cast macro results from `size_t` to `uint32_t` explicitly.
- **Clipboard**: large payloads are written across `POLLOUT` ticks; do not close the send fd on `EAGAIN`.
- **Weather**: `auto_locate` is best effort and uses the cached location on failure. `[shell].offline_mode = true` disables all HTTP traffic. Never log coordinates or location names.
- **Brightness**: keep the service instance stable across reloads and hotplug, since consumers hold raw pointers. Check `BrightnessDisplay::controllable` before exposing writes. Read the `brightness` sysfs file, not `actual_brightness`.

## Plugins

- Plugins are trusted Luau code: installing one is equivalent to running a user-owned script. There is no permission broker, capability sandbox, consent prompt, or path sandbox. Luau sandboxing exists only for VM and global-state isolation.
- A distributable plugin is a directory with one static `plugin.toml`. There are no single-file `define{}` plugins and no manifest defaults in script code.
- Canonical ids are `author/plugin` and `author/plugin:entry`. Entry types are `[[widget]]`, `[[shortcut]]`, `[[service]]`, `[[desktop_widget]]`, `[[launcher_provider]]`, `[[panel]]`, and `[[layer]]`.
- Defaults live once in `plugin.toml`. Plugin code reads them with `nythera.getConfig(key)` or `barWidget.getConfig(key)`; undeclared keys are loud misses. Plugin-level overrides live under `[plugin_settings."author/plugin"]`.
- Plugin bar widgets use the normal named `[widget.<name>]` settings, so separate widget names can reuse one plugin entry with different settings. Entry-specific settings belong to the owning surface's config model.
- Git and path source lifecycle belongs to `PluginManager` and `plugin_git.*`. Source names and plugin ids are validated flat identifiers before they touch paths. Git sources use an app-managed repo cache and exported runtime files (no sparse checkout); `path` sources are for local development.
- Changes to the plugin-facing API (bindings in `src/scripting/luau_host.cpp` and `src/scripting/plugin_bindings.cpp`, the `ui.*` prelude in `src/scripting/ui_prelude.h`, or the entry callbacks the host calls) require matching updates to `docs/plugin-api.json` and Nythera's plugin documentation. The external official-plugins repository still supplies `noctalia.d.luau`; that upstream API is translated at source ingestion as described in [REBRAND.md](REBRAND.md).

## Theming and Style

- Style constants live in `src/ui/style.h`, colors in `src/ui/palette.h`. Controls aim for a rounded, comfortably spaced look tuned for a native 1080p desktop.
- Terminology: `Palette` is a resolved RGB set, `ColorRole` a named slot, `ColorSpec` a role or fixed value. User-facing strings say "color role"; "theme" refers to the `[theme]` palette subsystem.
- `FixedPaletteMode` carries `dark` and `light`, each with a UI `palette` and a nested `terminal` palette.
- Palette JSON (community, custom, `nythera theme --theme-json`) carries a top-level integer `version`; a file without one is format 1. Format 2 keys are the color role names (`on_primary`, `surface_container`, terminal `selection_fg`), listed once in `kFixedPaletteJsonKeys`. `src/theme/fixed_palette_json.cpp` upgrades older formats in memory on read and rejects newer ones. Community palette requests send `?version=<kPaletteFormatVersion>`, and the API answers with at most that format, converting newer palettes down so older shells keep working.
- Built-in template hooks live beside their template (for example `assets/templates/fuzzel/apply.sh`). Community templates are fetched from `https://api.noctalia.dev/templates` and cached under `$XDG_STATE_HOME/nythera/community-templates`. Template application runs on a background worker.
- Glyph names resolve through `src/render/text/glyph_registry.cpp`: curated aliases, then `U+`/`0x` codepoints, then Tabler names from `tabler.json`. C++ glyph literals belong in the curated registry.

## Launcher

Results dispatch activation only to the provider that produced them. Fuzzy scoring uses the vendored `third_party/fzy`. Desktop entry scanning follows XDG data-dir precedence.
