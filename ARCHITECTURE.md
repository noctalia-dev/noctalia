# Architecture

This file describes how Noctalia is put together and the invariants reviewers enforce. For code style, testing, commit conventions, and the project layout, see [CONTRIBUTING.md](CONTRIBUTING.md).

## Event Loop

The main loop is poll based (`MainLoop` plus the `PollSource` interface). Every event source (Wayland, D-Bus, inotify, timers) exposes file descriptors that are polled together.

A new service implements `PollSource`, is added as an `Application` member, and is returned from `buildPollSources()`.

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

**Library** (`lib/`): `core`, `i18n`, `render`, `ui`, `util`, and `wayland` form the `noctalia-ui` static library (`noctalia_ui_dep`). `lib/` is self-contained so other Noctalia applications can carry a verbatim copy: `lib/meson.build` finds its own dependencies, flags, and `wayland-scanner`, and `lib/` holds the protocol XMLs it generates (`lib/protocols/`), the assets it loads (`lib/assets/`: the glyph font and its metadata), and its vendored code (`lib/third_party/wuffs`). A consumer only runs `subdir('lib')`. The library builds without `src/` on its include path, so it never includes shell code, shell-only protocol headers, or build metadata such as the git revision.

The library holds no application policy; the application supplies it at startup (the shell does this in `configureSharedLibrary()`, `src/app/shell_library.cpp`):

- **Identity**: `setApplicationIdentity()` (`core/application_identity.h`) names the XDG directories, the log file, the installed asset directory, and the `<PREFIX>_*` environment variables the library reads.
- **Assets**: `paths::setAssetRoots()` (`core/files/resource_paths.h`) accepts application-selected application/library directories before the first asset lookup. Explicit roots bypass shell discovery and require no translation catalog. The library checks its own font and metadata; applications validate their additional required files. The shell retains its existing discovery when roots are not supplied.
- **Text**: the library owns no translations. `setUiStrings()` (`core/ui_strings.h`) provides the few strings it shows on its own (key names, the `Select` placeholder).
- **Image decoding**: the core has the `Image` control, the texture caches, and the `ImageDecoder` interface (`render/core/image_decoder.h`) but no codecs. `codecs::installImageDecoder()` installs the opt-in codec pack (`noctalia_ui_codecs_dep`, `render/codecs/`: Wuffs, ICO, WebP, JPEG XL, librsvg, runtime-loaded libheif, encoders, thumbnails); another application can install its own decoder instead.

Reusable native windows use `ToplevelSurface`: borrowed `xdgToplevel()`/`xdgSurface()` access, native actions and configure-committed maximized/fullscreen/activated state. Register role lifecycle hooks before initialization: create application-owned attachments before the first commit and release them before role destruction, including failed initialization. Hook owners outlive the surface; release hooks must not throw. Decoration protocols and policy belong to the application. Close stops/notifies once, and early close/destruction cannot revive initialization.

`WaylandConnection::setKeyboardFocusCallback()` observes application focus independently of TextInputService registration. Seat queries and keyboard-enter serial are updated before internal text-input notification, followed by the application callback. Attaching/detaching text input preserves that callback; applications detach borrowed services before destroying them. Native lifecycle/actions and two-window focus were checked in an isolated core consumer under private Umbriel in debug and ASan+UBSan with leak detection. Existing lifetime/dispatcher/composition/text-input suites also pass; Files decoration integration and live IME-provider checks remain separate.

Code that only the shell uses belongs in `src/`, even when it looks generic.

**Shell and widget code** (`src/shell/`) uses only controls from `lib/ui/controls/` (`Label`, `Button`, `Glyph`, `Box`, `Separator`, ...) and the shell-owned controls in `src/shell/controls/` (`MarkdownView`, `SearchPicker`, `ColorPicker`, `GlyphPicker`). It never creates raw scene nodes such as `TextNode`, `RectNode`, or `IconNode`. `InputArea` is the only exception. Use `Box` for backgrounds, `Separator` for dividers, and `Label` for text.

**Controls** (`lib/ui/controls/`) own the raw nodes.

- `Label::measure(Renderer&)` must be called after setting text and font, before positioning.
- Use `VirtualGridView` for fixed-size cell grids and `VirtualListView` for variable-height lists.
- A control composes its child nodes in its constructor, implements layout, and animates through `AnimationManager`.

**Composition**: prefer the declarative `ui::` builders (`ui::row`, `ui::column`, `ui::label`, `ui::button`, ...) wherever a builder exists. Use direct construction and setters only for properties or lifecycle work the builders cannot express. Primitive controls may construct their own internal nodes directly; they do not depend on the aggregate builder factory for that.

## Scene Graph and Input

`Node` is the scene graph base class. `InputArea` is a `Node` subclass for pointer hit testing, and `InputDispatcher` routes pointer events through the tree. `InputArea::setAcceptedButtons()` takes masks built with `InputArea::buttonMask(...)`, not raw `BTN_*` codes.

`InputArea::observeDestruction()` registers one notification per owner-lifetime token. A shared or reparented area notifies every surviving dispatcher when destroyed; expired owners are skipped and pruned on registration. Dispatchers invalidate their tokens at the start of destruction and cannot be copied or moved. This keeps focus, hover, pointer capture and gesture targets from retaining deleted areas without calling into destroyed dispatchers. Scene mutation, observer registration and destruction stay on the scene thread.

Text paste uses `TextClipboard::requestClipboardText()`. Providers complete once on the UI thread, either inline or asynchronously; the default implementation uses the provider's cached text, while transports that need a read override it without blocking. The application owns transport, selection revisions, cancellation and limits. `Input` accepts a reply only while its lifetime, edit revision, focus, enabled state and composition state still permit it. Edits, caret/selection changes, focus loss, text-input deactivation and newer paste requests invalidate older requests. Accepted paste is one undoable edit, with the usual password restrictions. Change callbacks may destroy the input; the callback's value remains valid during the call.

`Input::composing()` exposes active preedit state; `setOnCompositionChange()` reports start/end transitions before value/submission callbacks, without restarting on each preedit update. Reset, replacement, blur and keyboard cancellation report the end too. Callbacks can destroy the input, so later callbacks stop when its lifetime expires. `selectRange()` takes UTF-8 byte offsets, advancing offsets inside a character to its next boundary and preserving anchor/caret direction. `setClearButtonWidth()` accepts a positive logical width; zero restores the existing height-based target.

`InputDispatcher::setFocus(area, true)` can preserve an in-flight IME edit through `TextInputService::transferFocusedClient()`. The transfer is accepted only inside an edit callback, for the active source and a different target client on the same active surface. The application forwards the current edit to that target. The service changes the client without resetting preedit or sending protocol disable/enable; subsequent edits and state commits use the new client. Other focus changes retain the normal lifecycle. The edit-transfer scope closes on both normal return and exceptions.

Shared context menus accept optional literal trailing shortcut hints and an opt-in destructive Error tone; key binding and action policy stay with the application. Preserve the existing row metrics and neutral palette defaults. `setHighlightedIndex(entryCount())` means no highlight; Down/Up then starts at the first/last interactive entry, skipping headings, separators and disabled rows. Pointer-leave clearing is opt-in through `setClearHighlightOnLeave(true)`. `ContextMenuPopupRequest` exposes `clearHighlightOnLeave` and accepts `initialHighlight == entries.size()` for no initial selection; its default still highlights the first action. Home/End skip inactive boundary rows. `tests/context_menu_control_test.cpp` exercises real text measurement, constrained/scaled columns, navigation and dispatcher pointer transitions.

Button label budgets measure custom visible layout artwork alongside the built-in glyph. Hidden and non-layout children consume no space; stacked artwork does not reduce horizontal label width. Keep application tab/focus palettes outside this sizing behavior. Stepper clamps parsed long values before narrowing; invalid or long-overflow edits restore the current value. Modified editing keys reach Input, retaining clipboard and undo/redo behavior. Applications can set step-button tooltips through the existing `incrementButton()`/`decrementButton()` accessors. `tests/stepper_input_test.cpp` covers numeric boundaries, validation and actual Input editing/commit behavior.

VirtualGridView adapters can opt into background/area selection, custom pointer hit shapes and bounded decoration. Files and other applications own selection identities and contour policy. `setTileVisible` distinguishes actual visibility from overscan before binding/layout; `releaseTile` retires inactive pooled resources during Layout with a Renderer. Binding replaces prior content and tile destructors own final cleanup. The borrowed adapter must outlive its grid. Adapter replacement clears bindings/geometry and sends false visibility to the old adapter. Existing adapters retain no-op defaults.

Use `itemAtScene` for full-cell drop hit testing and `itemRect` for visible clipped scene bounds. `firstVisibleIndex`/`firstVisibleOffset` and aligned `scrollToIndex` restore row/pixel anchors; default scrolling remains minimal. `cancelPointerGesture` stops area timers and clears capture before notifying the adapter; owned drag scrolling preserves that gesture, while ordinary scrolling and obsolete area identities cancel it. `ScrollView::contentViewportRect` excludes padding, borders and scrollbar lanes in scene coordinates. Optional `setHorizontalOverflow` leaves header/scrollbar synchronization to the owner. Pending target reveal supports both axes and rejects removed targets. `tests/virtual_grid_view_contract_test.cpp` covers resource, geometry and gesture lifecycles with real text measurements.

## Rendering

- `RenderContext` owns scene traversal and delegates drawing to a `RenderBackend`. Construct backends with `createDefaultRenderBackend()`.
- Texture allocation goes through `TextureManager`, constructed with `createDefaultTextureManager()`.
- Shell and widget code works with typed render concepts (`TextureId`, `TextureHandle`, `RenderFramebuffer`, `RenderSurfaceTarget`, nodes, draw primitives). Raw `GLuint` or EGL handles stay inside the render infrastructure.
- `lib/render` draws generic primitives only (rect, text, image, glyph, spinner, blur, fullscreen texture and tint) and never includes shell code. `RenderContext` does not own shader programs or include `render/programs/*`. Style and data structs live in neutral headers under `render/core/`.
- A new generic primitive is a GLES program in `lib/render/programs/`, a typed `RenderBackend` draw method, and ownership by `GlesRenderBackend`.
- Shell-only drawing (wallpaper, visualizers, graphs, weather effects, transitions) lives in `src/shell/render/`: a `CustomDrawNode` subclass whose `draw()` fetches its GLES program with `RenderBackend::extension<T>()`. The program derives from `RenderExtension`; the backend creates it on first use and destroys or abandons it with its own GPU objects.
- `Surface::setOverlayNode()` renders a node after the scene root in surface coordinates, for passes that must cover the whole surface (the desktop-widget wallpaper mask).
- Text is rasterized with Pango and Cairo, uploaded through `TextureManager`, and drawn with `RenderBackend::drawGlyph()`.
- Async images are consumer scoped through `Image::setSourceFileAsync()` and `AsyncTextureCache::subscribeReady()`; there is no fan-out from `Application`.

**Rectangle shapes**: the rect shader is generic. Use per-corner `CornerShape::Convex` or `CornerShape::Concave` with `Radii` and `RectInsets logicalInset`. Panel and bar policy stays in shell code, not in shader enum names. Screen-corner overlays use the shell's `ScreenCornerNode` custom draw node.

CPU image resizing uses `resizeToRgba()` in `lib/render/core/image_resize.*`. `ResizeSourceLayout::Rgb` accepts tightly packed three-channel sRGB input and returns opaque RGBA, alongside the existing four-channel layouts. stb requires matching channel counts, so RGB is filtered into the final output allocation and expanded backwards in place; do not add a full-size RGBA source conversion. Keep decoding, source limits and worker scheduling in their owning providers. `tests/image_resize_test.cpp` exercises channel order, sRGB/alpha filtering, packed buffers and invalid inputs.

SharedTextureCache accepts immutable prepared RGBA payloads through `acquirePrepared`, keyed by exact nonempty source/revision identities, actual dimensions and mipmap mode. Generation policy, source validation and scheduling belong to the provider. Hold pixels in a shared immutable `LoadedImageFile` (an aliasing shared pointer can reuse a provider-owned object). No copying, decoding or file access occurs during acquisition. Uploads, revision switches, source invalidation and callbacks run from dispatch; prepared leases keep their ready image until its replacement is resident. Reset/release discards queued work, and final release evicts residency. `invalidatePrepared` retires currently held source revisions; use a new revision for changed content. Path coverage behavior remains separate and unchanged.

Prepared payloads stay shared while held so GPU reload can upload them again without decoding. Shared EGL consumers reuse one upload; without sharing, backends own separate textures. `preparedBytes` deduplicates held payload addresses; `residentBytes` estimates GPU bytes including mip levels. These are accounting counters, not provider policy or an eviction budget. `tests/shared_texture_cache_test.cpp` verifies real GLES readback/lifetimes with an explicit absolute private Wayland socket argument. It skips without that argument, so ordinary headless runs never connect to the active desktop. Run the built test only against an isolated compositor, including for sanitizer checks.

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

- Every panel (attached, detached, floating, centered) owns its own `noctalia-panel` layer surface and draws its own background, shadow, blur, and reveal animation.
- "Attached" is a placement mode only: the panel's own surface is anchored to the bar's edge at the bar's layer, with a clipped reveal. It is not hosted in the bar surface.
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

- `WaylandConnection` is protocol transport only, with no compositor-specific branches, and `lib/wayland` never includes shell code. It binds the globals a plain xdg-toplevel client needs (outputs and xdg-output, seat, shm, compositor, xdg-shell, activation, cursor shape, viewporter, fractional scale, text input).
- Shell-only and compositor-specific globals (layer shell, background-effect blur, toplevels, workspaces, clipboard, capture, idle, session lock, gamma, output management, Hyprland focus grab) live in `src/shell/wayland/` as `WaylandExtension`s fetched with `WaylandConnection::extension<T>()`. The connection replays already advertised globals to a new extension and releases every extension before it disconnects. Extensions whose globals must be bound in the initial registry roundtrip are created before `connect()`. Compositor behavior goes through `CompositorPlatform`.
- `LayerSurface`, its `wl_surface` registry, and blur regions belong to the shell (`LayerShell`, `BackgroundEffectGlobals`). Library popups never name a layer surface: a non-xdg parent supplies a `PopupParentAttach` hook that assigns the `xdg_popup` its parent (`LayerShell::popupAttach()`), and a parent that refuses keyboard focus supplies a `PopupKeyboardGrab` (`LayerShell::popupKeyboardGrab()`).
- Output make, model, serial, fixed scale, and transform come from an extension through `setOutputMetadataSource()`; without one, the configured scale falls back to the mode/logical ratio and `wl_output.scale`.
- Prefer passing `CompositorPlatform&` over passing both `WaylandConnection&` and `CompositorPlatform&`.
- Detect compositors with `compositors::detect()`, `isHyprland()`, `isNiri()`, `isSway()`, and `isMango()` from `compositor_detect.h`; do not sniff environment variables again.
- Logout goes through `uwsm stop` when UWSM manages the compositor, otherwise compositor-native logout commands. Niri goes through `NiriRuntime` IPC, not the CLI.

## Configuration and State

- Schema structs and enums live in `src/config/config_types.*`. `ConfigService` owns lifecycle and hot reload. Override persistence lives in `src/config/config_overrides.cpp`.
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

- **Idle**: `IdleManager` runs `ext_idle_notifier_v1` behaviors. Each `[idle.behavior.*]` has a `command` and an optional `resume_command`. `noctalia:dpms-off` gets an automatic `noctalia:dpms-on` on resume. `pre_action_fade_seconds` drives `IdleGraceOverlay` on overlay layer surfaces.
- **PipeWire**: access SPA pod arrays with the stable `SPA_POD_ARRAY_*` macros from `spa/pod/pod.h`, not `spa_pod_get_array_full()` (added in PipeWire 1.6, absent in 1.4). Cast macro results from `size_t` to `uint32_t` explicitly.
- **Clipboard**: large payloads are written across `POLLOUT` ticks; do not close the send fd on `EAGAIN`.
- **Weather**: `auto_locate` is best effort and uses the cached location on failure. `[shell].offline_mode = true` disables all HTTP traffic. Never log coordinates or location names.
- **Brightness**: keep the service instance stable across reloads and hotplug, since consumers hold raw pointers. Check `BrightnessDisplay::controllable` before exposing writes. Read the `brightness` sysfs file, not `actual_brightness`.

## Plugins

- Plugins are trusted Luau code: installing one is equivalent to running a user-owned script. There is no permission broker, capability sandbox, consent prompt, or path sandbox. Luau sandboxing exists only for VM and global-state isolation.
- A distributable plugin is a directory with one static `plugin.toml`. There are no single-file `define{}` plugins and no manifest defaults in script code.
- Canonical ids are `author/plugin` and `author/plugin:entry`. Entry types are `[[widget]]`, `[[shortcut]]`, `[[service]]`, `[[desktop_widget]]`, `[[launcher_provider]]`, `[[panel]]`, and `[[layer]]`.
- Defaults live once in `plugin.toml`. Plugin code reads them with `noctalia.getConfig(key)` or `barWidget.getConfig(key)`; undeclared keys are loud misses. Plugin-level overrides live under `[plugin_settings."author/plugin"]`.
- Plugin bar widgets use the normal named `[widget.<name>]` settings, so separate widget names can reuse one plugin entry with different settings. Entry-specific settings belong to the owning surface's config model.
- Git and path source lifecycle belongs to `PluginManager` and `plugin_git.*`. Source names and plugin ids are validated flat identifiers before they touch paths. Git sources use an app-managed repo cache and exported runtime files (no sparse checkout); `path` sources are for local development.
- Changes to the plugin-facing API (bindings in `src/scripting/luau_host.cpp` and `src/scripting/plugin_bindings.cpp`, the `ui.*` prelude in `src/scripting/ui_prelude.h`, or the entry callbacks the host calls) require a matching update to `noctalia.d.luau` in the official-plugins repository.

## Theming and Style

- Style constants live in `lib/ui/style.h`, colors in `lib/ui/palette.h`. Controls aim for a rounded, comfortably spaced look tuned for a native 1080p desktop.
- Terminology: `Palette` is a resolved RGB set, `ColorRole` a named slot, `ColorSpec` a role or fixed value. User-facing strings say "color role"; "theme" refers to the `[theme]` palette subsystem.
- `FixedPaletteMode` carries `dark` and `light`, each with a UI `palette` and a nested `terminal` palette.
- Palette JSON (community, custom, `noctalia theme --theme-json`) carries a top-level integer `version`; a file without one is format 1. Format 2 keys are the color role names (`on_primary`, `surface_container`, terminal `selection_fg`), listed once in `kFixedPaletteJsonKeys`. `src/theme/fixed_palette_json.cpp` upgrades older formats in memory on read and rejects newer ones. Community palette requests send `?version=<kPaletteFormatVersion>`, and the API answers with at most that format, converting newer palettes down so older shells keep working.
- Built-in template hooks live beside their template (for example `assets/templates/fuzzel/apply.sh`). Community templates are fetched from `https://api.noctalia.dev/templates` and cached under `$XDG_STATE_HOME/noctalia/community-templates`. Template application runs on a background worker.
- Glyph names resolve through `lib/render/text/glyph_registry.cpp`: curated aliases, then `U+`/`0x` codepoints, then Tabler names from `lib/assets/fonts/tabler.json`. C++ glyph literals belong in the curated registry.

## Launcher

Results dispatch activation only to the provider that produced them. Fuzzy scoring uses the vendored `third_party/fzy`. Desktop entry scanning follows XDG data-dir precedence.
