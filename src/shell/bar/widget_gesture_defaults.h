#pragma once

#include "config/config_types.h"
#include "shell/bar/widget_action.h"

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace noctalia::bar {

  // Per-type gesture metadata, keyed by widget type so both the runtime and the settings GUI read
  // the same source. The GUI only ever has the type string, never a Widget instance, which is why
  // these are free functions rather than virtuals on Widget.

  // Layer 1: applies to every widget type.
  [[nodiscard]] std::span<const GestureBinding> builtinGestureDefaults() noexcept;

  // What the parts of a bar no widget covers do out of the box. Separate from the widget layers:
  // there is no widget there, so `settings-open-widget` has nothing to open.
  [[nodiscard]] std::span<const GestureBinding> deadZoneGestureDefaults() noexcept;

  // Layer 2: what a widget type declares for itself, empty when it declares none. For a plugin
  // [[widget]] type this is the manifest's `actions` table, so plugin widgets get defaults the same
  // way built-in ones do. `config` is read only by types whose defaults genuinely differ per
  // setting, e.g. a volume widget bound to the microphone needs the mic verbs.
  //
  // The returned bindings borrow from the type tables and from the loaded manifest, both of which
  // outlive a resolve pass.
  [[nodiscard]] std::vector<GestureBinding>
  gestureDefaultsForType(std::string_view type, const WidgetConfig* config = nullptr);

  // Layers 1 and 2 merged, as config-shaped strings. Used to show defaults in the settings editor.
  [[nodiscard]] std::unordered_map<std::string, std::string>
  defaultActionsForType(std::string_view type, const WidgetConfig* config = nullptr);

  // Gestures a widget type handles on its individual items (workspace pills, taskbar tasks, tray
  // icons) rather than as a whole. They are never bindable and the settings editor omits them.
  [[nodiscard]] GestureMask reservedGesturesForType(std::string_view type) noexcept;

} // namespace noctalia::bar
