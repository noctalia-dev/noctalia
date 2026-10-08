#pragma once

#include "config/config_types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Schema (validity authority) for widget settings. Unlike a section Schema<T>,
// whose fields bind to struct members, a widget's settings are a dynamic map
// keyed by widget type — so the schema is a per-type list of WidgetSettingField.
// This describes WHAT a setting is (type, range, default, allowed values); how
// it is presented (label, control, grouping, visibility) is the shell registry's
// concern and layers on top of this.
namespace noctalia::config::schema {

  // The data/validation type of a setting value. Coarser than the shell's control
  // kind (which also distinguishes File/Folder/Glyph/Select etc.) — File/Folder/
  // Glyph all validate as String; Select validates as Enum; ColorSpec as Color.
  enum class WidgetSettingType : std::uint8_t {
    Bool,
    Int,
    Double,
    OptionalDouble,
    String,
    StringList,
    StringMap,
    Enum,
    Color,
  };

  // One widget setting's schema: its key, value type, default, optional numeric
  // bounds, and (for Enum) the allowed values. The single source of truth used to
  // validate a `[widget.<name>]` / `[desktop_widgets].widget.*` setting.
  struct WidgetSettingField {
    std::string key;
    WidgetSettingType type = WidgetSettingType::String;
    WidgetSettingValue defaultValue = std::string{};
    std::optional<double> minValue;
    std::optional<double> maxValue;
    std::optional<double> step;
    std::vector<std::string> enumValues; // allowed values when type == Enum
    // An unset value inherits the owning bar's same-named key (its monitor override first), so presence is
    // meaningful: an explicit value equal to `defaultValue` is still an override. Bool settings only.
    bool inheritsFromBar = false;
  };

  using WidgetSettingSchema = std::vector<WidgetSettingField>;

} // namespace noctalia::config::schema
