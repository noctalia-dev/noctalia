#pragma once

// Internal to the settings registry: entry builders shared by the per-section sources and the
// section builders that buildSettingsRegistry() calls. Not part of the settings_registry.h API.

#include "config/config_types.h"
#include "config/schema/field.h"
#include "i18n/i18n.h"
#include "shell/bar/widget_gesture.h"
#include "shell/settings/settings_registry.h"
#include "ui/palette.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace settings::registry {

  inline constexpr int kBarMarginMax = 4096;
  inline constexpr float kBarCornerRadiusMax = 80.0F;

  // Builds a slider whose bounds come from the shared schema Range, the same
  // constant the parser clamps with. This keeps the UI range and the config clamp
  // one source. `integerValue` (write as int64) stays explicit: it is a UI/write
  // choice, not implied by the range's numeric type (e.g. transition_duration).
  template <typename V, typename T>
  SliderSetting sliderFor(V value, const noctalia::config::schema::Range<T>& range, bool integerValue) {
    return SliderSetting{
        static_cast<double>(value), static_cast<double>(range.min.value()), static_cast<double>(range.max.value()),
        static_cast<double>(range.step.value()), integerValue
    };
  }

  template <typename T, std::size_t N> SelectSetting enumSelect(const EnumOption<T> (&options)[N], T selected) {
    std::vector<SelectOption> opts;
    opts.reserve(N);
    std::string selectedValue;
    for (const auto& option : options) {
      std::string key(option.key);
      if (option.value == selected) {
        selectedValue = key;
      }
      opts.push_back(SelectOption{std::move(key), i18n::tr(option.labelKey)});
    }
    if (selectedValue.empty() && N > 0) {
      selectedValue = std::string(options[0].key);
    }
    return SelectSetting{std::move(opts), std::move(selectedValue)};
  }

  SelectSetting asSegmented(SelectSetting setting);
  SelectSetting
  plainSelect(std::initializer_list<std::pair<std::string_view, std::string_view>> items, std::string_view selected);
  SelectSetting positionSelect(std::string_view selected);

  SliderSetting barCornerSlider(std::int32_t value);
  SliderSetting barReservedSlider(double value, double maxValue, double step, bool integer);
  StepperSetting barMarginStepper(std::int32_t value);
  LengthSetting barMarginEndsSetting(const LengthSpec& value);
  StepperSetting barPanelOverlapStepper(std::int32_t value);
  std::optional<int> radiusStepperValue(const std::optional<double>& value);
  int radiusStepperFallback(const std::optional<double>& value);
  std::string_view barAutoHideMode(bool autoHide, bool smartAutoHide);
  SelectSetting autoHideModeSelect(std::string_view mode, std::vector<std::string> smartPath);
  // What the dead zone does for a gesture nobody has bound, shown as the row's placeholder so an
  // empty field never reads as "does nothing".
  std::string deadZoneDefault(noctalia::bar::Gesture gesture);

  ColorSpecPickerSetting
  colorSpecPicker(const std::optional<ColorSpec>& selected, bool allowNone, std::string noneLabel = {});
  ColorSpecPickerSetting colorSpecPicker(const ColorSpec& selected, bool allowNone = false, std::string noneLabel = {});

  SettingEntry makeEntry(
      SettingsSection section, std::string group, std::string title, std::string subtitle,
      std::vector<std::string> path, SettingControl control, const std::string& tags = {}, bool advanced = false
  );

  // Position select for a panel. Floating pins it on screen; attached honors only
  // positions on the bar's edge (see shell::attachedPositionAlign).
  SettingEntry panelPositionEntry(
      SettingsSection section, std::string group, std::string_view panelKey, std::string_view labelKey,
      std::string_view descKey, const std::string& current
  );
  // Click override: when on, a click on the panel's bar widget opens it near the widget
  // instead of at its Position.
  SettingEntry panelOpenNearClickEntry(
      SettingsSection section, std::string group, std::string_view panelKey, std::string_view labelKey,
      std::string_view descKey, bool nearClick, PanelPlacement ShellConfig::PanelConfig::* placement,
      std::string ShellConfig::PanelConfig::* position
  );

  // Section builders, appended in this order by buildSettingsRegistry(). Entries render grouped
  // by first appearance, so the order here is the order users see.
  void addAppearanceSettings(std::vector<SettingEntry>& entries, const Config& cfg, const RegistryEnvironment& env);
  void addWallpaperSettings(std::vector<SettingEntry>& entries, const Config& cfg, const RegistryEnvironment& env);
  void addTemplateSettings(std::vector<SettingEntry>& entries, const Config& cfg);
  void addDockSettings(std::vector<SettingEntry>& entries, const Config& cfg, const RegistryEnvironment& env);
  void addPanelSettings(std::vector<SettingEntry>& entries, const Config& cfg);
  void addLauncherSettings(std::vector<SettingEntry>& entries, const Config& cfg);
  void addClipboardAndWallpaperPanelSettings(std::vector<SettingEntry>& entries, const Config& cfg);
  void addControlCenterSettings(std::vector<SettingEntry>& entries, const Config& cfg);
  void addDesktopSettings(std::vector<SettingEntry>& entries, const Config& cfg);
  void addSecuritySettings(std::vector<SettingEntry>& entries, const Config& cfg);
  void addLockScreenSettings(std::vector<SettingEntry>& entries, const Config& cfg, const RegistryEnvironment& env);
  void addShellSettings(std::vector<SettingEntry>& entries, const Config& cfg, const RegistryEnvironment& env);
  void addScreenshotSettings(std::vector<SettingEntry>& entries, const Config& cfg);
  void addWindowSwitcherSettings(std::vector<SettingEntry>& entries, const Config& cfg);
  void addOsdSettings(std::vector<SettingEntry>& entries, const Config& cfg, const RegistryEnvironment& env);
  void addKeybindSettings(std::vector<SettingEntry>& entries, const Config& cfg);
  void addNiriSettings(std::vector<SettingEntry>& entries, const Config& cfg, const RegistryEnvironment& env);
  void addUmbrielSettings(std::vector<SettingEntry>& entries, const Config& cfg, const RegistryEnvironment& env);
  void addSystemSettings(std::vector<SettingEntry>& entries, const Config& cfg, const RegistryEnvironment& env);
  void addLocationSettings(std::vector<SettingEntry>& entries, const Config& cfg, const RegistryEnvironment& env);
  void addCalendarSettings(std::vector<SettingEntry>& entries, const Config& cfg);
  void addServiceSettings(std::vector<SettingEntry>& entries, const Config& cfg, const RegistryEnvironment& env);
  void addPowerSettings(std::vector<SettingEntry>& entries, const Config& cfg);
  void addIdleSettings(std::vector<SettingEntry>& entries, const Config& cfg);
  void addHookSettings(std::vector<SettingEntry>& entries, const Config& cfg);
  void addNotificationSettings(std::vector<SettingEntry>& entries, const Config& cfg, const RegistryEnvironment& env);
  void addBarSettings(std::vector<SettingEntry>& entries, const Config& cfg, const RegistryEnvironment& env);
  void
  addBarMonitorOverrideSettings(std::vector<SettingEntry>& entries, const Config& cfg, const RegistryEnvironment& env);

} // namespace settings::registry
