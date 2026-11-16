#include "shell/settings/settings_registry_common.h"

#include "config/config_types.h"
#include "config/schema/ranges.h"
#include "i18n/i18n.h"
#include "shell/bar/widget_gesture.h"
#include "shell/bar/widget_gesture_defaults.h"
#include "shell/settings/color_spec_picker.h"
#include "shell/settings/settings_registry.h"
#include "ui/palette.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace settings::registry {
  namespace {

    std::string pathText(const std::vector<std::string>& path) {
      std::string out;
      for (const auto& part : path) {
        if (!out.empty()) {
          out.push_back('.');
        }
        out += part;
      }
      return out;
    }

  } // namespace

  SliderSetting barCornerSlider(std::int32_t value) {
    SliderSetting s{value, 0.0F, kBarCornerRadiusMax, 1.0F, true};
    return s;
  }

  std::string deadZoneDefault(noctalia::bar::Gesture gesture) {
    for (const auto& binding : noctalia::bar::deadZoneGestureDefaults()) {
      if (binding.gesture == gesture) {
        return std::string(binding.action);
      }
    }
    return {};
  }

  SliderSetting barReservedSlider(double value, double maxValue, double step, bool integer) {
    return SliderSetting{value, 0.0F, maxValue, step, integer};
  }

  SelectSetting asSegmented(SelectSetting setting) {
    setting.segmented = true;
    return setting;
  }

  StepperSetting barMarginStepper(std::int32_t value) {
    return StepperSetting{
        .value = static_cast<int>(value),
        .minValue = 0,
        .maxValue = kBarMarginMax,
        .step = 1,
        .valueSuffix = "px",
    };
  }

  LengthSetting barMarginEndsSetting(const LengthSpec& value) {
    return LengthSetting{
        .value = value,
        .maxPixels = kBarMarginMax,
        .maxPercent = static_cast<int>(noctalia::config::schema::kBarMarginEndsPercentRange.max.value()),
    };
  }

  StepperSetting barPanelOverlapStepper(std::int32_t value) {
    return StepperSetting{
        .value = static_cast<int>(value),
        .minValue = -2,
        .maxValue = 3,
        .step = 1,
        .valueSuffix = "px",
    };
  }

  std::optional<int> radiusStepperValue(const std::optional<double>& value) {
    if (!value.has_value()) {
      return std::nullopt;
    }
    return std::clamp(static_cast<int>(std::lround(*value)), 0, 80);
  }

  int radiusStepperFallback(const std::optional<double>& value) { return radiusStepperValue(value).value_or(8); }

  SelectSetting
  plainSelect(std::initializer_list<std::pair<std::string_view, std::string_view>> items, std::string_view selected) {
    std::vector<SelectOption> opts;
    opts.reserve(items.size());
    for (const auto& [value, labelKey] : items) {
      opts.push_back(SelectOption{std::string(value), i18n::tr(labelKey)});
    }
    return SelectSetting{std::move(opts), std::string(selected)};
  }

  std::string_view barAutoHideMode(bool autoHide, bool smartAutoHide) {
    if (smartAutoHide) {
      return "smart";
    }
    if (autoHide) {
      return "on";
    }
    return "off";
  }

  SelectSetting autoHideModeSelect(std::string_view mode, std::vector<std::string> smartPath) {
    auto select = asSegmented(plainSelect(
        {{"off", "settings.options.bar.auto-hide.off"},
         {"on", "settings.options.bar.auto-hide.on"},
         {"smart", "settings.options.bar.auto-hide.smart"}},
        mode
    ));
    select.linkedPath = std::move(smartPath);
    select.groupedCommit = [](std::string_view value, const std::vector<std::string>& primaryPath) {
      auto companionPath = primaryPath;
      companionPath.back() = "smart_auto_hide";
      return std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>{
          {primaryPath, ConfigOverrideValue{value == "on"}},
          {std::move(companionPath), ConfigOverrideValue{value == "smart"}},
      };
    };
    return select;
  }

  SelectSetting positionSelect(std::string_view selected) {
    return asSegmented(plainSelect(
        {{"top", "settings.options.edge.top"},
         {"bottom", "settings.options.edge.bottom"},
         {"left", "settings.options.edge.left"},
         {"right", "settings.options.edge.right"}},
        selected
    ));
  }

  ColorSpecPickerSetting
  colorSpecPicker(const std::optional<ColorSpec>& selected, bool allowNone, std::string noneLabel) {
    return ColorSpecPickerSetting{
        .roles = {},
        .selectedValue = optionalColorSpecConfigValue(selected),
        .allowNone = allowNone,
        .allowCustomColor = true,
        .noneLabel = std::move(noneLabel),
    };
  }

  ColorSpecPickerSetting colorSpecPicker(const ColorSpec& selected, bool allowNone, std::string noneLabel) {
    return colorSpecPicker(std::optional<ColorSpec>{selected}, allowNone, std::move(noneLabel));
  }

  SettingEntry makeEntry(
      SettingsSection section, std::string group, std::string title, std::string subtitle,
      std::vector<std::string> path, SettingControl control, const std::string& tags, bool advanced
  ) {
    std::string searchText = std::string(settingsSectionId(section))
        + " "
        + group
        + " "
        + title
        + " "
        + subtitle
        + " "
        + pathText(path)
        + " "
        + tags;
    if (advanced) {
      searchText += " advanced";
    }
    return SettingEntry{
        .section = section,
        .group = std::move(group),
        .title = std::move(title),
        .subtitle = std::move(subtitle),
        .path = std::move(path),
        .control = std::move(control),
        .advanced = advanced,
        .searchText = StringUtils::normalizeForSearch(searchText),
    };
  }

  SettingEntry panelPositionEntry(
      SettingsSection section, std::string group, std::string_view panelKey, std::string_view labelKey,
      std::string_view descKey, const std::string& current
  ) {
    return makeEntry(
        section, std::move(group), i18n::tr(labelKey), i18n::tr(descKey),
        {std::string("shell"), std::string("panel"), std::string(panelKey) + "_position"},
        plainSelect(
            {{"auto", "settings.options.panel-position.auto"},
             {"center", "settings.options.screen-position.center"},
             {"top_left", "settings.options.screen-position.top-left"},
             {"top_center", "settings.options.screen-position.top-center"},
             {"top_right", "settings.options.screen-position.top-right"},
             {"center_left", "settings.options.screen-position.center-left"},
             {"center_right", "settings.options.screen-position.center-right"},
             {"bottom_left", "settings.options.screen-position.bottom-left"},
             {"bottom_center", "settings.options.screen-position.bottom-center"},
             {"bottom_right", "settings.options.screen-position.bottom-right"}},
            current
        ),
        "panel position screen anchor corner edge floating attached align bottom right"
    );
  }

  SettingEntry panelOpenNearClickEntry(
      SettingsSection section, std::string group, std::string_view panelKey, std::string_view labelKey,
      std::string_view descKey, bool nearClick, PanelPlacement ShellConfig::PanelConfig::* placement,
      std::string ShellConfig::PanelConfig::* position
  ) {
    auto e = makeEntry(
        section, std::move(group), i18n::tr(labelKey), i18n::tr(descKey),
        {"shell", "panel", std::string("open_near_click_") + std::string(panelKey)}, ToggleSetting{nearClick},
        "open near click trigger widget along bar position anchor"
    );
    e.visibleWhen = [placement, position](const Config& c) {
      const auto& panel = c.shell.panel;
      return panel.*placement == PanelPlacement::Attached
          || (panel.*placement == PanelPlacement::Floating && panel.*position == "auto");
    };
    return e;
  }

} // namespace settings::registry
