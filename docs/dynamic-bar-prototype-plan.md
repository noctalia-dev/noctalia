# Dynamic bar: initial prototype

Status: initial prototype implemented for visual review, 4 November 2026.

Updated direction: the widget editor is the sole source of bar contents. No composite
clock or automatically inserted media, visualiser or transport widgets.

## Saved starting point

- Baseline branch: `codex/bar-baseline-2026-11-04`, commit `c53f851f6`.
- Working branch: `dev` (renamed from `codex/dynamic-bar-prototype` on 6 November 2026).
- `main` remains at its previous commit. The baseline includes the eight previously
  uncommitted files, including configuration isolation and the surface-mode selector.
- Design references: [Bar Ideas](https://wiki.kangy.uk/nythera/nythera-shell/bar-ideas)
  and [Bar](https://wiki.kangy.uk/nythera/nythera-shell/bar).

## First result to evaluate

A rounded, translucent bar with three functional sections. It can use a continuous
background or three genuine section backgrounds. The centre smoothly resizes around
its configured widgets. Workspace-aware behaviour follows as a
separate step. The screen-wrapping frame is a later experiment using the same pieces.

Use the selected palette, current widget membership, native content/font scaling,
and existing bar placement. Rounded silhouettes and restrained emphasis should do
the visual work; animation should communicate a change of state.

## 1. Establish reliable section geometry

Inspect `Bar::buildScene`, layout, palette updates, input regions, shadows and attached
panel geometry together before changing backgrounds. The current islands implementation
hides the continuous background and relies on configured capsules; that alone does
not guarantee one complete background per section.

- Give start, centre and end sections their own measured background bounds in islands
  mode, enclosing every module assigned to that section. Empty sections paint nothing.
- Support top, bottom, left and right using main/cross-axis geometry. Start/end become
  top/bottom for vertical placement; changing modes must not change the selected edge.
- Keep the existing continuous-bar geometry and corner policy for full mode.
- Scope input and compositor blur regions to the actual island shapes. Transparent
  gaps must not intercept desktop clicks or request a full-width blur.
- Reuse palette roles, configured opacity, radii and outlines. Palette changes update
  both modes without rebuilding their identity around fixed colours.
- Reuse the existing Full Bar / Floating Islands setting and per-monitor overrides.

Checkpoint: switch layouts at every edge and confirm section grouping, corners,
spacing, attached popups, click-through gaps and configuration persistence.

## 2. Prototype the dynamic centre

Animate the measured size of the existing centre section with `AnimationManager`.
The bar must not insert, replace, or relocate widgets. Clock, media, visualiser and
transport functionality belongs to existing configurable widgets and their settings.

- Smooth section growth and shrinkage when configured content changes size or visibility.
- Retarget interrupted transitions from the current extent.
- Keep the section centred along the selected edge and clip content to its animated bounds.
- Bound expansion by the space left by the outer sections.
- Follow global motion preferences and stop scheduling frames when settled.
- Retain existing accordion behaviour for hover expansion; do not duplicate its controls.
- Expose Dynamic Centre in Bar → Effects and per-monitor settings; default it to off.

Checkpoint: verify full/islands layouts and all four orientations using the existing
widget arrangement. Further media presentations can be assembled in the widget editor.

## 3. Workspace-driven repositioning — dropped

Dropped by Kangy on 6 November 2026. Moving a bar section along its selected edge
in response to window positions adds layout complexity, shifts neighbouring content,
and provides too little practical benefit. Do not implement this as a future phase.

Keep the existing workspace widget sizing, app-icon membership and smooth resizing
of its surrounding island. Those content-driven size changes are already implemented
and are separate from the rejected repositioning idea.

## Future idea: manually positioned freeform islands

Previously discussed and restored to the backlog on 6 November 2026: allow islands
to be manually clicked and dragged to chosen positions along the bar, beyond fixed
start/centre/end placement. This is not implemented. Editing controls, collision or
snapping rules and position persistence still need design, with any added options
exposed in the settings GUI.

This idea remains wanted. It is separate from existing widget/group reordering,
content-driven island resizing and the rejected automatic workspace/window-following
repositioning above.

## 4. Evaluate a screen wrap separately

After the centre and section geometry are accepted, prototype rounded rails around
the desktop opening, with configurable modules on each side and per-monitor enablement.
Decide how adjoining bars share corners, input and reserved space before joining rails.
Use the same widgets, palette and animation controller where appropriate.

Do not assume a single transparent monitor-sized rendering surface is cheap. Compare
edge surfaces and shared frame rendering on the actual four-monitor setup before
settling the architecture. This phase is not required for the first usable prototype.

## Validation and delivery

Implement each checkpoint as a separate commit on the prototype branch. Use focused
geometry/state tests where they cover meaningful failures, build the shell, and exercise
the relevant Settings and runtime paths. Keep preview changes in the fork's isolated
configuration; switching Git branches does not restore user configuration.

Measure baseline and prototype with the same build type and monitor setup: idle,
playback, repeated expansion and workspace switching. Record RAM, process-attributable
VRAM where available, CPU and frame timing. Report attribution limitations and actual
measurements rather than a promised resource budget. Check that idle animation stops
and hidden visualisers stop scheduling frames.

Review after phase 2: visual judgement belongs to Kangy. Continue with workspace
adaptation and the frame only after the initial behaviour provides a useful direction.

## Initial prototype review notes

- Replaced the old capsule-only islands implementation with three measured section backgrounds.
- Added generic centre-size animation around configured widgets; no media or spectrum is injected.
- Extended the existing Active Window widget with an anticlockwise vertical title,
  animated length and title crossfades. Show Vertical Title is a widget Settings toggle.
- Native build and focused config/animation/settings tests pass. Full and islands modes
  were rendered at all four edges during development, then restored to the left edge.
- Current widget membership is retained. Existing media and visualiser widgets remain
  removable/configurable through the widget editor.
- Full idle/playback RAM, VRAM and frame-time benchmarking remains pending. Early
  resource samples included a now-removed composite widget and are not final results.
- Workspace-driven repositioning was subsequently dropped on 6 November 2026. Screen wrapping was implemented separately; see the current Bar documentation.

## Monitor-local grouping follow-up

The approved CNS-style monitor-local workspace/application display uses Nythera's
existing grouped Taskbar widget. The running isolated profile now uses `taskbar`
in the former `workspaces` slot, with `show_all_outputs = false`,
`group_by_workspace = true`, `only_active_workspace = false` and icon group content.
Other configured modules and per-monitor bar positions were preserved. All controls
remain in the native Taskbar widget editor, with grouping controls under Advanced.
See the Taskbar user documentation for the reusable configuration.

Active configuration validation and the taskbar regression test passed. Live Umbriel
workspace/window snapshots confirmed monitor-local membership; DP-1's rendered groups
matched its two workspaces and three running windows. DP-3 had a fullscreen game over
its bar during inspection, so that visual check was deferred without changing it.

This establishes monitor-local content. Automatic empty/occupied layout adaptation
and the optional screen wrap remain separate later work.
