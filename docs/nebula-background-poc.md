# Nebula panel background

The shared native background system for [issue #18](https://github.com/Nythera-Dev/Nythera/issues/18) retains the expanded Workspace Window Map baseline: near-black material, restrained crimson sheets, delicate wisps and very slow movement. It uses procedural rendering rather than an image texture.

## Settings

**Settings → Nebula Haze** contains the master switch, independent **Static / Animated** and **Opaque / Transparent** choices, and individual surface switches. Opaque and Animated remain the defaults. The global animation switch also holds haze still. Turning haze off restores each host's normal background without forgetting its haze options.

Transparent mode composes crimson emission over the native translucent backing; it does not apply opacity to the scene or foreground subtree. The renderer retains each material's resolved tint and opacity, including snapshot backdrops, without imposing an additional opacity cap. Solid native materials remain solid; translucency follows their existing opacity settings. The Window Map retains its established Soft/Glass backing choice. Text, controls, window previews, borders and images retain their normal treatment.

Existing configuration paths are retained to preserve saved choices:

```toml
[shell.panel]
haze_enabled = true
haze_mode = "animated"       # "static" or "animated"
haze_background = "opaque"   # "opaque" or "transparent"

[shell.panel.haze_surfaces]
launcher = false
notifications = true
"workspace-map-window" = true
```

Missing per-surface entries default to enabled; the master switch takes precedence. These choices hot reload and persist through the normal Settings override system. A surface switch addresses a native host type, such as notifications, rather than an individual short-lived toast. Desktop widget instance IDs and plugin panel/layer IDs are also supported; discovered and saved IDs appear in the surface list. New dynamic IDs become visible when rebuilding the Settings page.

The window-preview pause button remains independent. There is no haze keyboard shortcut. Theme colouring and an intensity slider remain future work; this rollout introduces neither a new palette nor a speed control.

## Native surface coverage

- PanelManager hosts: Control Centre, Launcher, Clipboard History, Polkit, Session, Tray Drawer, Wallpaper picker, Setup Wizard, compact Window Map, Window Inspector and registered native/plugin panels.
- Native windows: Settings and expanded Workspace Window Map, with a generic profile for other native toplevel hosts.
- Notifications, OSD, Window Switcher, dock and dock menus, tooltips, tray menus, context menus and selection dropdowns.
- Separate file, colour, glyph and search picker popups, plus generic native dialog popups.
- Desktop widget backgrounds, the widget editor inspector, lock-screen form/widget materials, screenshot toolbars, annotation toolbars and plugin-layer root materials.

Each surface instance gets an independent pattern seed; the expanded map keeps its original seed. Embedded dialogs share the configuration of their owning native surface. Bars and islands never receive haze. Where the attached-panel contour is drawn by a shared bar/screen-wrap surface, haze is explicitly restricted to the panel-body bounds.

Invisible input shields, hot-corner targets, screen-corner/reservation surfaces, wallpaper/backdrop and window-capture planes are infrastructure rather than panel backgrounds. Screenshot/annotation canvases, dimming scrims and captured images retain their original rendering. Chromeless plugin layers or lock-screen layouts without a filled native material remain clear: the system does not invent a rectangle behind them. These hosts can render haze when their existing native background material is present.

Settings keeps haze behind its main window backing and foreground UI. While the global switch and Settings surface switch are both enabled, decorative group/search-result cards use a 20% tint instead of 40%, and the otherwise solid sidebar uses a 40% tint. Borders, controls, selected states and window backing opacity are unchanged. Existing translucent Settings sidebars stay clear. Disabling haze restores the original fills, including on configuration reload.

## Rendering and lifecycle

`SurfaceHaze` under `src/render/animation/` owns one shared approximately 30 Hz timer using the existing TimerManager. Native Surface hosts own their profile, local elapsed time and seed. A participant is ready only while running, configured, unsuspended, displaying a background material, and free of a queued render or outstanding compositor frame callback. Hidden/closed surfaces and static, disabled or suspended effects do not keep the timer running. Time does not jump across an inactive interval.

Haze ticks request paint directly without invoking unrelated frame-tick callbacks. Window preview capture and widget data update schedules remain independent. Static backgrounds issue no continuous redraw requests; they may be evaluated again when other content legitimately repaints, avoiding an extra framebuffer/cache system.

Panel/dialog Box materials are tagged at their existing background node. Other hosts tag their existing Box/Flex material explicitly. RenderContext passes neutral style data to the existing rectangle and screen-frame draw paths. Both GLES programs include one shared `nebula_glsl.h` function, using existing backend/context ownership and GPU resource sharing. There is no per-panel shader, texture allocation, intermediate framebuffer, additional blur pass or animation timer.

The native rectangle/contour shaders continue to own dimensions, corner shape, clipping, border and coverage. The shared function preserves the baseline colour coefficients, spatial scale, fine detail, drift and fixed spatial dither. Its premultiplied result changes background material only.

## Validation

Focused coverage lives in `surface_haze_test.cpp`, `surface_haze_render_test.cpp`, the configuration tests and the existing workspace-map tests. The GPU test runs the actual GLES rectangle and contour programs and checks static determinism, motion, independent seeds, transparent alpha, convex/concave/chamfer coverage, borders and unchanged bar pixels outside the panel body.

An incremental two-job build and nine focused Meson tests passed: `surface_haze`, `surface_haze_render`, `config_schema_roundtrip`, `config_override_mutation`, `workspace_map`, `animation_manager`, `context_menu_popup`, `surface_blur_region` and `panel_resize`. English catalogue and touched documentation formatting checks passed. The GPU check also verifies premultiplied source-over composition against a known background.

An isolated native Wayland harness on Umbriel exercised two independently configured toplevel surfaces. Over settled 1.6-second intervals, animated surfaces rendered 44–45 times; individually disabled surfaces rendered zero while the other continued. Static (opaque and transparent), global-off, suspended and closed states produced zero ongoing renders and left no haze timer armed. Resume restored animation. Haze frames invoked zero unrelated content frame-tick callbacks. Captures confirmed desktop visibility through the background and byte-identical opaque foreground interiors. The real expanded Window Map and Settings Haze page both opened and closed successfully. Test windows were closed afterward and live shell configuration was unchanged.

This validates the shared native path and Settings layout; it does not mean every integrated popup, lock-screen layout or attached-panel placement was manually exercised. The contour GPU regression covers unchanged bar pixels and native corner coverage.

Comprehensive multi-panel performance and power benchmarking remains deferred to issue #18. Focused lifecycle checks establish scheduling behaviour, not whole-shell power usage or compositor-wide performance.
