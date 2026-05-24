#pragma once

#include "config/config_service.h"
#include "ui/controls/color_swatch_preview.h"
#include "ui/palette.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace settings {

  struct ToggleSetting {
    bool checked = false;
    bool enabled = true; // false renders the toggle in a disabled/non-interactive state
  };

  struct SelectOption {
    std::string value;
    std::string label;
    std::string description = {};
    ColorSwatchPreview preview = {};
  };

  struct SelectSetting {
    std::vector<SelectOption> options;
    std::string selectedValue;
    bool clearOnEmpty = false;
    bool segmented = false;      // render as Segmented pill group instead of dropdown Select
    bool integerValue = false;   // option values are numeric strings; write as int64_t to config
    float preferredWidth = 0.0f; // 0 = default settings dropdown width
  };

  struct SearchPickerSetting {
    std::vector<SelectOption> options;
    std::string selectedValue;
    std::string placeholder;
    std::string emptyText;
    float preferredHeight = 240.0f;
  };

  struct SliderSetting {
    SliderSetting() = default;
    SliderSetting(float valueIn, float minValueIn, float maxValueIn, float stepIn, bool integerValueIn)
        : value(valueIn), minValue(minValueIn), maxValue(maxValueIn), step(stepIn), integerValue(integerValueIn) {}

    float value = 0.0f;
    float minValue = 0.0f;
    float maxValue = 1.0f;
    float step = 0.01f;
    bool integerValue = false;
    // Optional: when set, called with the user's just-committed value and returns extra overrides
    // to commit atomically alongside it. Use for cross-field constraints (e.g. linked sliders).
    std::function<std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>(double committedValue)>
        linkedCommit;
  };

  enum class TextSettingBrowseMode : std::uint8_t {
    None = 0,
    SelectFolder,
    OpenFile,
  };

  struct TextSetting {
    std::string value;
    std::string placeholder;
    float width = 0.0f; // 0 = use default
    TextSettingBrowseMode browseMode = TextSettingBrowseMode::None;
    /// When browseMode == OpenFile, optional filter (e.g. `{".wav", ".ogg"}`); empty allows any file.
    std::vector<std::string> browseFileExtensions;
  };

  struct OptionalNumberSetting {
    std::optional<double> value;
    double minValue = 0.0;
    double maxValue = 1.0;
    std::string placeholder;
  };

  struct OptionalStepperSetting {
    std::optional<int> value;
    int minValue = 0;
    int maxValue = 100;
    int step = 1;
    int fallbackValue = 0;
    std::string unsetLabel;
    std::string customLabel;
  };

  /// Integer stepper (always has a value; no unset/custom segmented UI).
  struct StepperSetting {
    int value = 0;
    int minValue = 0;
    int maxValue = 100;
    int step = 1;
    /// Appended to the value display (e.g. `"s"` → `5s`). Empty = plain number.
    std::string valueSuffix = {};
  };

  struct ListSetting {
    std::vector<std::string> items;
    // When non-empty, the add UI presents a Select limited to these options (minus already-added values)
    // instead of a free-form text input, and row labels resolve to the option's friendly label.
    // Useful when the catalog of valid values is known.
    std::vector<SelectOption> suggestedOptions = {};
  };

  struct ShortcutListSetting {
    std::vector<ShortcutConfig> items;
    std::vector<SelectOption> suggestedOptions = {};
    std::size_t maxItems = 0;
  };

  struct KeybindListSetting {
    std::vector<KeyChord> items;
    std::size_t maxItems = 0;
  };

  struct SessionPanelActionsSetting {
    std::vector<SessionPanelActionConfig> items;
  };

  struct IdleBehaviorsSetting {
    std::vector<IdleBehaviorConfig> items;
  };

  struct MultiSelectSetting {
    std::vector<SelectOption> options;
    std::vector<std::string> selectedValues;
    bool requireAtLeastOne = false; // disable removing the last selected entry
  };

  struct ButtonSetting {
    std::string label;
    std::function<void()> action;
    std::string glyph;
  };

  struct ColorSpecPickerSetting {
    std::vector<ColorRole> roles;
    std::string selectedValue;
    bool allowNone = false;
    bool allowCustomColor = true;
    std::string noneLabel;
  };

  using SettingControl = std::variant<
      ToggleSetting, SelectSetting, SliderSetting, TextSetting, OptionalNumberSetting, OptionalStepperSetting,
      StepperSetting, ListSetting, ShortcutListSetting, KeybindListSetting, SessionPanelActionsSetting,
      IdleBehaviorsSetting, MultiSelectSetting, ButtonSetting, ColorSpecPickerSetting, SearchPickerSetting>;

  struct SettingVisibilityCondition {
    std::vector<std::string> path;
    std::vector<std::string> values;
  };

  struct SettingVisibility {
    SettingVisibility() = default;
    SettingVisibility(std::vector<std::string> pathIn, std::vector<std::string> valuesIn)
        : all{SettingVisibilityCondition{std::move(pathIn), std::move(valuesIn)}} {}
    explicit SettingVisibility(std::vector<SettingVisibilityCondition> conditions) : all(std::move(conditions)) {}

    std::vector<SettingVisibilityCondition> all;
  };

  struct SettingEntry {
    std::string section;
    std::string group;
    std::string title;
    std::string subtitle;
    std::vector<std::string> path;
    SettingControl control;
    bool advanced = false;
    std::string searchText;
    std::optional<SettingVisibility> visibleWhen;
  };

  // Runtime conditions that gate optional sections (e.g. compositor-specific features).
  struct RegistryEnvironment {
    bool niriBackdropSupported = false;             // hide niri backdrop entries when false
    bool niriOverviewTypeToLaunchSupported = false; // show niri-only type-to-launch integration
    bool ddcutilAvailable = false;                  // disable ddcutil toggle when ddcutil is not on PATH
    bool gammaControlAvailable = false;             // hide night-light entries when gamma control is unavailable
    std::vector<SelectOption> availableOutputs;     // monitor selectors available on this machine
    std::vector<SelectOption> communityPalettes;
    std::vector<SelectOption> customPalettes;
    std::vector<SelectOption> communityTemplates;
    std::vector<SelectOption> fontFamilies;
  };

  [[nodiscard]] const BarConfig* findBar(const Config& cfg, std::string_view name);
  [[nodiscard]] const BarMonitorOverride* findMonitorOverride(const BarConfig& bar, std::string_view match);
  [[nodiscard]] std::vector<std::string> barNames(const Config& cfg);
  [[nodiscard]] std::vector<SettingEntry> buildSettingsRegistry(
      const Config& cfg, const BarConfig* selectedBar, const BarMonitorOverride* selectedMonitorOverride = nullptr,
      const RegistryEnvironment& env = {}
  );
  [[nodiscard]] std::string normalizedSettingQuery(std::string_view query);
  [[nodiscard]] bool matchesNormalizedSettingQuery(const SettingEntry& entry, std::string_view normalizedQuery);
  [[nodiscard]] bool matchesSettingQuery(const SettingEntry& entry, std::string_view query);
  [[nodiscard]] bool isBarMonitorOverrideSettingPath(const std::vector<std::string>& path);
  [[nodiscard]] bool settingEntryMatchesBarNavigation(
      const SettingEntry& entry, std::string_view selectedBarName, std::string_view selectedMonitorOverride
  );
  [[nodiscard]] std::string barSettingContentSectionKey(const SettingEntry& entry);
  [[nodiscard]] std::string_view sectionGlyph(std::string_view section);

} // namespace settings
