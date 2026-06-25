#pragma once

#include "config/config_types.h"
#include "config/schema/widget_schema.h"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scripting {
  struct ManifestField;
  struct PluginEntry;
  class PluginTranslationCatalog;
} // namespace scripting

namespace settings {

  enum class WidgetReferenceKind : std::uint8_t {
    BuiltIn,
    Named,
    Unknown,
    Plugin, // a plugin [[widget]] entry from the plugin registry
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
    WidgetReferenceKind kind = WidgetReferenceKind::Unknown;
  };

  struct WidgetPickerEntry {
    std::string value; // for Plugin entries this is the entry id "author/plugin:entry"
    std::string label;
    std::string description;
    std::string icon;
    WidgetReferenceKind kind = WidgetReferenceKind::Unknown;
  };

  // How a setting is rendered in the settings UI. Distinct from the schema's
  // WidgetSettingType (the value/validation type): File/Folder/Glyph all carry a
  // String value, Select carries an Enum value, ColorSpec carries a Color value.
  enum class WidgetControlKind : std::uint8_t {
    Bool,
    Int,
    Double,
    OptionalDouble,
    String,
    File,
    Folder,
    Glyph,
    StringList,
    StringMap,
    Select,
    ColorSpec,
  };

  enum class WidgetSettingGroup : std::uint8_t {
    Widget,
    Presentation,
    Runtime,
    Grouping,
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
    std::vector<WidgetSettingVisibilityCondition> any; // visible if any alternative matches (empty = unconstrained)
    std::vector<WidgetSettingVisibilityCondition> all; // additionally requires every condition to match

    WidgetSettingVisibility() = default;
    WidgetSettingVisibility(std::string key, std::vector<std::string> values)
        : any{WidgetSettingVisibilityCondition{std::move(key), std::move(values)}} {}
    WidgetSettingVisibility(std::initializer_list<WidgetSettingVisibilityCondition> alternatives) : any(alternatives) {}
  };

  // Presentation overlay for one widget setting. The validity half (key, value
  // type, default, range, allowed enum values) lives in `schema` — the single
  // source the config layer validates against; everything else here is UI only.
  struct WidgetSettingSpec {
    noctalia::config::schema::WidgetSettingField schema; // validity: key/type/default/range/enumValues
    WidgetControlKind control = WidgetControlKind::String;
    std::string labelKey;
    std::string descriptionKey;
    std::string literalLabel;       // when non-empty, used verbatim instead of tr(labelKey)
    std::string literalDescription; // when non-empty, used verbatim instead of tr(descriptionKey)
    bool literalLabels = false;     // when true, option.labelKey holds a literal label (not an i18n key)
    WidgetSettingGroup group = WidgetSettingGroup::Widget;
    std::vector<WidgetSettingSelectOption> options; // value+label; values mirror schema.enumValues
    bool advanced = false;
    bool segmented = false;              // applies when control == Select
    bool integerValue = false;           // applies when control == Select
    bool stepper = false;                // applies when control == Int
    std::string valueSuffix;             // applies when control == Int && stepper
    bool allowCustomColor = true;        // applies when control == ColorSpec
    std::vector<std::string> extensions; // applies when control == File
    std::optional<WidgetSettingVisibility> visibleWhen;
    bool horizontalBarOnly = false; // hide on left/right bars (e.g. media album-art-only)
  };

  // The schema (validation) value type behind a UI control kind.
  [[nodiscard]] noctalia::config::schema::WidgetSettingType schemaTypeForControl(WidgetControlKind control);

  [[nodiscard]] const std::vector<WidgetTypeSpec>& widgetTypeSpecs();
  [[nodiscard]] bool isBuiltInWidgetType(std::string_view type);
  // Whether `type` names a plugin [[widget]] entry ("author/plugin:entry").
  [[nodiscard]] bool isPluginWidgetType(std::string_view type);
  [[nodiscard]] bool widgetTypeRequiresNamedConfig(std::string_view type);
  [[nodiscard]] std::string widgetTypeForReference(const Config& cfg, std::string_view name);
  [[nodiscard]] std::string titleFromWidgetKey(std::string_view key);
  [[nodiscard]] WidgetReferenceInfo
  widgetReferenceInfo(const Config& cfg, std::string_view name, bool includeManifestVersion = true);
  [[nodiscard]] std::vector<WidgetPickerEntry> widgetPickerEntries(const Config& cfg);
  [[nodiscard]] std::vector<WidgetSettingSpec> commonWidgetSettingSpecs(std::string_view shellFontFamily);
  [[nodiscard]] std::vector<WidgetSettingSpec> widgetSettingSpecs(
      std::string_view type, std::string_view shellFontFamily, bool supportsTaskbarWorkspaceGrouping = true
  );
  // Config-aware variant: for a plugin [[widget]] type, returns the manifest-driven
  // settings. Falls back to the type-only specs otherwise.
  [[nodiscard]] std::vector<WidgetSettingSpec> widgetSettingSpecs(
      std::string_view type, const WidgetConfig* config, std::string_view shellFontFamily,
      bool supportsTaskbarWorkspaceGrouping = true
  );
  // Build settings specs from a plugin entry's declared setting schema.
  [[nodiscard]] std::vector<WidgetSettingSpec> manifestSettingSpecs(
      const std::vector<scripting::ManifestField>& fields,
      const scripting::PluginTranslationCatalog* translations = nullptr
  );
  [[nodiscard]] std::vector<WidgetSettingSpec> pluginPanelShellSettingSpecs(const scripting::PluginEntry& entry);

  // Schema projection (the validity half of the specs), consumed by the config
  // layer (e.g. `config validate`). For plugin widgets the type alone resolves the
  // manifest, so the config arg is no longer required for them.
  [[nodiscard]] noctalia::config::schema::WidgetSettingSchema widgetSettingSchema(std::string_view type);
  [[nodiscard]] noctalia::config::schema::WidgetSettingSchema
  widgetSettingSchema(std::string_view type, const WidgetConfig* config);
  [[nodiscard]] std::optional<noctalia::config::schema::WidgetSettingField>
  findWidgetSettingField(std::string_view widgetType, std::string_view settingKey);

  [[nodiscard]] std::optional<WidgetSettingSpec>
  findWidgetSettingSpec(std::string_view widgetType, std::string_view settingKey);
  [[nodiscard]] std::optional<WidgetSettingSpec>
  findWidgetSettingSpec(std::string_view widgetType, std::string_view settingKey, const WidgetConfig* config);
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
