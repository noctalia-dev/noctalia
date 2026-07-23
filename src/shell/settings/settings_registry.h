#pragma once

#include "config/config_types.h"
#include "core/input/key_chord.h"
#include "ui/controls/color_swatch_preview.h"
#include "ui/palette.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace settings {

  enum class SettingsSection : std::uint8_t {
    Appearance,
    Wallpaper,
    Templates,
    Desktop,
    Dock,
    Panels,
    Launcher,
    ControlCenter,
    Notifications,
    Osd,
    Shell,
    Keybinds,
    Security,
    System,
    Services,
    Location,
    Power,
    Hooks,
    Niri,
    Bar,
    Plugins,
  };

  struct SettingsSectionDescriptor {
    SettingsSection section;
    std::string_view id;
    std::string_view glyph;
    bool sidebar = true;
    // Show in the sidebar even with no registry entries (fully custom-content section).
    bool alwaysShow = false;
  };

  struct ToggleSetting {
    bool checked = false;
    bool enabled = true; // false renders the toggle in a disabled/non-interactive state
  };

  struct SelectOption {
    std::string value;
    std::string label;
    std::string description;
    ColorSwatchPreview preview = {};
    std::string tooltip;
  };

  enum class SelectValueType : std::uint8_t {
    String,
    Integer,
    Boolean,
  };

  struct SelectSetting {
    std::vector<SelectOption> options;
    std::string selectedValue;
    bool clearOnEmpty = false;
    bool segmented = false;                              // render as Segmented pill group instead of dropdown Select
    SelectValueType valueType = SelectValueType::String; // storage type for option values
    float preferredWidth = 0.0f;                         // 0 = default settings dropdown width
    std::vector<std::string> linkedPath;                 // companion path for groupedCommit / override reset
    std::function<std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>(
        std::string_view selectedValue, const std::vector<std::string>& primaryPath
    )>
        groupedCommit;
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
    template <
        typename Value, typename MinValue, typename MaxValue, typename Step,
        typename = std::enable_if_t<
            std::is_arithmetic_v<Value>
            && std::is_arithmetic_v<MinValue>
            && std::is_arithmetic_v<MaxValue>
            && std::is_arithmetic_v<Step>>>
    SliderSetting(Value valueIn, MinValue minValueIn, MaxValue maxValueIn, Step stepIn, bool integerValueIn)
        : value(static_cast<double>(valueIn)), minValue(static_cast<double>(minValueIn)),
          maxValue(static_cast<double>(maxValueIn)), step(static_cast<double>(stepIn)), integerValue(integerValueIn) {}

    // Trailing invert control for signed radius-style sliders: the slider shows the
    // magnitude (0..max) and an inline toggle carries the sign (negative = concave).
    // Reserve renders an equal-width empty slot so sibling sliders stay column-aligned.
    enum class InvertSlot : std::uint8_t { None, Reserve, Toggle };

    double value = 0.0;
    double minValue = 0.0;
    double maxValue = 1.0;
    double step = 0.01;
    bool integerValue = false;
    InvertSlot invertSlot = InvertSlot::None;
    bool invertEnabled = true; // only meaningful when invertSlot == Toggle
    std::string valueSuffix;
    // Optional: when set, called with the user's just-committed value and returns extra overrides
    // to commit atomically alongside it. Use for cross-field constraints (e.g. linked sliders).
    std::function<std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>(double committedValue)>
        linkedCommit;
  };

  /// Dual-thumb slider for a low/high pair on one axis. `entry.path` is the low (e.g. activity)
  /// value; `highPath` is the high (e.g. critical) value. Both participate in override/reset together.
  struct RangeSliderSetting {
    double lowValue = 0.0;
    double highValue = 1.0;
    double minValue = 0.0;
    double maxValue = 1.0;
    double step = 0.01;
    bool integerValue = false;
    std::string valueSuffix;
    std::vector<std::string> highPath;
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
    /// When the current value is empty, open the file picker here if the path exists.
    std::string browseFallbackDirectory;
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
    std::string valueSuffix;
  };

  struct ListSetting {
    std::vector<std::string> items;
    // When non-empty, the add UI presents a Select limited to these options (minus already-added values)
    // instead of a free-form text input, and row labels resolve to the option's friendly label.
    // Useful when the catalog of valid values is known.
    std::vector<SelectOption> suggestedOptions;
  };

  struct StringMapSetting {
    std::unordered_map<std::string, std::string> entries;
    std::vector<std::string> suggestedKeys;
    std::string keyPlaceholder;
    std::string valuePlaceholder;
  };

  struct ShortcutListSetting {
    std::vector<ShortcutConfig> items;
    std::vector<SelectOption> suggestedOptions;
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

  struct NotificationFiltersSetting {
    std::vector<NotificationFilterConfig> items;
  };

  struct MultiSelectSetting {
    std::vector<SelectOption> options;
    std::vector<std::string> selectedValues;
    bool requireAtLeastOne = false; // disable removing the last selected entry
    bool persistUnselected = false; // persist the unchecked complement (denylist) instead of the selection
  };

  struct TemplateGridSetting {
    std::vector<SelectOption> options;
    std::vector<std::string> selectedValues;
    std::string emptyText;
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
      ToggleSetting, SelectSetting, SliderSetting, RangeSliderSetting, TextSetting, OptionalNumberSetting,
      OptionalStepperSetting, StepperSetting, ListSetting, ShortcutListSetting, KeybindListSetting,
      SessionPanelActionsSetting, IdleBehaviorsSetting, NotificationFiltersSetting, MultiSelectSetting,
      TemplateGridSetting, ButtonSetting, ColorSpecPickerSetting, SearchPickerSetting>;

  // Visibility predicate, evaluated against the same Config the registry was built from
  // (the registry rebuilds on every config change). Capture snapshot values or read the
  // passed Config; never capture references.
  using SettingVisibility = std::function<bool(const Config&)>;

  struct SettingEntry {
    SettingsSection section = SettingsSection::Appearance;
    std::string group;
    std::string title;
    std::string subtitle;
    std::vector<std::string> path;
    SettingControl control;
    bool advanced = false;
    std::string searchText;
    SettingVisibility visibleWhen; // empty = always visible
  };

  // Runtime conditions that gate optional sections (e.g. compositor-specific features).
  struct RegistryEnvironment {
    bool niriBackdropSupported = false;             // hide niri backdrop entries when false
    bool niriOverviewTypeToLaunchSupported = false; // show niri-only type-to-launch integration
    bool screencopySupported = false;               // lockscreen blurred desktop + screenshot features
    bool ddcutilAvailable = false;                  // disable ddcutil toggle when ddcutil is not on PATH
    bool systemdUserManaged = false;                // disable systemd app launching when the shell is not a user unit
    bool gammaControlAvailable = false;             // hide night-light entries when gamma control is unavailable
    bool greeterSyncAvailable = false;              // hide greeter appearance sync when greeter is not installed
    std::vector<SelectOption> availableOutputs;     // monitor selectors available on this machine
    bool batteryAvailable = false;
    bool systemBatteryAvailable = false;
    std::vector<SelectOption> batteryDeviceOptions;
    std::unordered_map<std::string, int> batteryWarningThresholds;
    std::vector<SelectOption> communityPalettes;
    std::vector<SelectOption> customPalettes;
    std::vector<SelectOption> communityTemplates;
    std::vector<SelectOption> fontFamilies;
    std::string shellAvatarPath;
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
  [[nodiscard]] std::span<const SettingsSectionDescriptor> settingsSectionDescriptors();
  [[nodiscard]] std::string_view settingsSectionId(SettingsSection section);
  [[nodiscard]] std::string settingsSectionLabelKey(SettingsSection section);
  [[nodiscard]] std::string_view sectionGlyph(SettingsSection section);
  [[nodiscard]] std::optional<SettingsSection> settingsSectionFromId(std::string_view id);

  // Returns a permutation of [0, count) that coalesces items sharing a group key so a group renders
  // exactly once, regardless of the order items were declared in. The first-appearance order of group
  // keys and the original order of items within a group are preserved. `keyFn(i)` yields item i's key.
  //
  // Both settings render passes emit a group header whenever an item's group differs from the previous
  // item's, so a stranded same-group item would otherwise produce a duplicate header. Route iteration
  // through this permutation to make group placement independent of declaration order.
  template <typename KeyFn>
  [[nodiscard]] std::vector<std::size_t> coalesceByGroupKey(std::size_t count, KeyFn&& keyFn) {
    std::unordered_map<std::string, std::size_t> rankByKey;
    std::vector<std::size_t> rankOf(count);
    std::size_t nextRank = 0;
    for (std::size_t i = 0; i < count; ++i) {
      auto [it, inserted] = rankByKey.try_emplace(keyFn(i), nextRank);
      if (inserted) {
        ++nextRank;
      }
      rankOf[i] = it->second;
    }
    std::vector<std::size_t> order(count);
    std::ranges::iota(order, std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return rankOf[a] < rankOf[b]; });
    return order;
  }

} // namespace settings
