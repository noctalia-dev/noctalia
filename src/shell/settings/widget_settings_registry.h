#pragma once

#include "config/config_service.h"
#include "scripting/scripted_widget_manifest.h"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace settings {

  enum class WidgetReferenceKind : std::uint8_t {
    BuiltIn,
    Named,
    Unknown,
    Preset, // a bundled scripted widget discovered via its Lua manifest
  };

  struct WidgetTypeSpec {
    std::string_view type;
    std::string_view labelKey;
    std::string_view glyph;
    bool supportsMultipleInstances = true;
    bool visibleInPicker = true;
  };

  struct WidgetReferenceInfo {
    std::string title;
    std::string detail;
    std::string badge;
    WidgetReferenceKind kind = WidgetReferenceKind::Unknown;
  };

  struct WidgetPickerEntry {
    std::string value;
    std::string label;
    std::string description;
    std::string icon;
    std::string script = {}; // asset-relative script path for Preset entries; empty otherwise
    WidgetReferenceKind kind = WidgetReferenceKind::Unknown;
  };

  enum class WidgetSettingValueType : std::uint8_t {
    Bool,
    Int,
    Double,
    OptionalDouble,
    String,
    StringList,
    Select,
    ColorSpec,
  };

  struct WidgetSettingSelectOption {
    std::string value;
    std::string labelKey; // i18n key, unless the owning spec sets `literalLabels` (then a literal label)
  };

  struct WidgetSettingVisibilityCondition {
    std::string key;
    std::vector<std::string> values;
  };

  struct WidgetSettingVisibility {
    std::vector<WidgetSettingVisibilityCondition> any;

    WidgetSettingVisibility() = default;
    WidgetSettingVisibility(std::string key, std::vector<std::string> values)
        : any{WidgetSettingVisibilityCondition{std::move(key), std::move(values)}} {}
    WidgetSettingVisibility(std::initializer_list<WidgetSettingVisibilityCondition> alternatives) : any(alternatives) {}
  };

  struct WidgetSettingSpec {
    std::string key;
    std::string labelKey;
    std::string descriptionKey;
    std::string literalLabel;       // when non-empty, used verbatim instead of tr(labelKey)
    std::string literalDescription; // when non-empty, used verbatim instead of tr(descriptionKey)
    bool literalLabels = false;     // when true, option.labelKey holds a literal label (not an i18n key)
    WidgetSettingValueType valueType = WidgetSettingValueType::String;
    WidgetSettingValue defaultValue = std::string{};
    std::optional<double> minValue;
    std::optional<double> maxValue;
    double step = 1.0;
    std::vector<WidgetSettingSelectOption> options;
    bool advanced = false;
    bool segmented = false;       // applies when valueType == Select
    bool allowCustomColor = true; // applies when valueType == ColorSpec
    std::optional<WidgetSettingVisibility> visibleWhen;
  };

  [[nodiscard]] const std::vector<WidgetTypeSpec>& widgetTypeSpecs();
  [[nodiscard]] bool isBuiltInWidgetType(std::string_view type);
  [[nodiscard]] bool widgetTypeRequiresNamedConfig(std::string_view type);
  [[nodiscard]] std::string widgetTypeForReference(const Config& cfg, std::string_view name);
  [[nodiscard]] std::string titleFromWidgetKey(std::string_view key);
  [[nodiscard]] WidgetReferenceInfo widgetReferenceInfo(const Config& cfg, std::string_view name);
  [[nodiscard]] std::vector<WidgetPickerEntry> widgetPickerEntries(const Config& cfg);
  [[nodiscard]] std::vector<WidgetSettingSpec> commonWidgetSettingSpecs();
  [[nodiscard]] std::vector<WidgetSettingSpec> widgetSettingSpecs(std::string_view type);
  // Config-aware variant: for scripted widgets whose `script` declares a Lua manifest,
  // returns the manifest-driven settings. Falls back to the type-only specs otherwise.
  [[nodiscard]] std::vector<WidgetSettingSpec> widgetSettingSpecs(std::string_view type, const WidgetConfig* config);
  // Build settings specs from a scripted widget's Lua manifest.
  [[nodiscard]] std::vector<WidgetSettingSpec> manifestSettingSpecs(const scripting::ScriptWidgetManifest& manifest);

  [[nodiscard]] std::optional<WidgetSettingSpec>
  findWidgetSettingSpec(std::string_view widgetType, std::string_view settingKey);
  [[nodiscard]] bool configOverrideValueMatchesWidgetSetting(
      const ConfigOverrideValue& overrideValue, const WidgetSettingValue& settingValue
  );
  [[nodiscard]] bool widgetOverrideValueMatchesRegistryDefault(
      std::string_view widgetType, std::string_view settingKey, const ConfigOverrideValue& overrideValue
  );
  [[nodiscard]] bool widgetSettingOverrideIsEffective(
      std::string_view widgetName, std::string_view settingKey, const Config& withOverride,
      const Config& withoutOverride
  );

} // namespace settings
