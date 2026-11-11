// Every typed widget definition validates itself lazily: `field()` and `WidgetDefinition::validate()`
// throw std::logic_error on the first resolve()/schemaFields() call, and WidgetFactory::create() does
// not catch. Without this test a malformed definition ships as an uncaught exception at bar build.
#include "shell/bar/widget_gesture_defaults.h"
#include "shell/bar/widgets/active_window_widget_definition.h"
#include "shell/bar/widgets/audio_visualizer_widget_definition.h"
#include "shell/bar/widgets/battery_widget_definition.h"
#include "shell/bar/widgets/bluetooth_widget_definition.h"
#include "shell/bar/widgets/brightness_widget_definition.h"
#include "shell/bar/widgets/caffeine_widget_definition.h"
#include "shell/bar/widgets/clipboard_widget_definition.h"
#include "shell/bar/widgets/clock_widget_definition.h"
#include "shell/bar/widgets/control_center_widget_definition.h"
#include "shell/bar/widgets/custom_button_widget_definition.h"
#include "shell/bar/widgets/keyboard_layout_widget_definition.h"
#include "shell/bar/widgets/launcher_widget_definition.h"
#include "shell/bar/widgets/lock_keys_widget_definition.h"
#include "shell/bar/widgets/media_widget_definition.h"
#include "shell/bar/widgets/network_widget_definition.h"
#include "shell/bar/widgets/nightlight_widget_definition.h"
#include "shell/bar/widgets/notification_widget_definition.h"
#include "shell/bar/widgets/power_profile_widget_definition.h"
#include "shell/bar/widgets/privacy_widget_definition.h"
#include "shell/bar/widgets/screenshot_widget_definition.h"
#include "shell/bar/widgets/session_widget_definition.h"
#include "shell/bar/widgets/settings_widget_definition.h"
#include "shell/bar/widgets/spacer_widget_definition.h"
#include "shell/bar/widgets/sysmon_widget_definition.h"
#include "shell/bar/widgets/taskbar_widget_definition.h"
#include "shell/bar/widgets/test_widget_definition.h"
#include "shell/bar/widgets/text_widget_definition.h"
#include "shell/bar/widgets/theme_mode_widget_definition.h"
#include "shell/bar/widgets/tray_widget_definition.h"
#include "shell/bar/widgets/volume_widget_definition.h"
#include "shell/bar/widgets/wallpaper_widget_definition.h"
#include "shell/bar/widgets/weather_widget_definition.h"
#include "shell/desktop/desktop_widget_settings_registry.h"
#include "shell/settings/widget_settings_registry.h"
#include "system/battery_warning_monitor.h"
#include "system/format_units.h"
#include "tests/test_check.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <print>
#include <set>
#include <string>
#include <string_view>
#include <utility>

// The battery definition's finalize hook reaches into the warning-threshold helper, which would drag
// UPower, notifications and i18n into a test about definition well-formedness. Threshold behavior is
// not under test here, so stand the helper in rather than link that tree.
int batteryWarningThresholdForSelector(
    const BatteryConfig& /*config*/, const UPowerService* /*upower*/, std::string_view /*selector*/
) {
  return 0;
}

namespace {

  // Exercises everything the settings registry and the widget factory ask of a definition: schema
  // projection, presentation projection, and resolution of both an absent config and one that spells
  // out every schema default. `type` is the string WidgetFactory::create() dispatches on, so a
  // definition that disagrees with it would silently never surface its settings.
  template <bool AllowEmpty = false, typename Accessor, typename... Context>
  void checkDefinition(std::string_view type, Accessor accessor, const Context&... context) {
    try {
      const auto& definition = accessor();
      if (definition.type != type) {
        std::println(stderr, "{}: definition declares the type '{}'", type, definition.type);
      }
      TEST_CHECK(definition.type == type);

      const auto schema = definition.schemaFields();
      if (schema.empty() && !AllowEmpty) {
        std::println(stderr, "{}", type);
      }
      TEST_CHECK(!schema.empty() || AllowEmpty);

      std::set<std::string> keys;
      for (const auto& field : schema) {
        if (field.key.empty()) {
          std::println(stderr, "{}", type);
        }
        TEST_CHECK(!field.key.empty());
        const bool inserted = keys.insert(field.key).second;
        if (!inserted) {
          std::println(stderr, "{}: {}", type, field.key);
        }
        TEST_CHECK(inserted); // duplicate field key
      }

      for (const auto& spec : definition.presentedSettingSpecs()) {
        if (spec.labelKey.empty() || spec.descriptionKey.empty()) {
          std::println(stderr, "{}: {}", type, spec.schema.key);
        }
        TEST_CHECK(!spec.labelKey.empty() && !spec.descriptionKey.empty());
      }

      // A config that repeats every schema default must resolve back to the plain Options defaults.
      WidgetConfig config;
      config.type = std::string(type);
      for (const auto& field : schema) {
        if (const auto* stringMap = std::get_if<WidgetSettingStringMap>(&field.defaultValue)) {
          config.tables[field.key] = *stringMap;
        } else {
          config.settings[field.key] = field.defaultValue;
        }
      }
      const bool defaultsMatch = definition.fieldValuesEqual(
          definition.resolve(&config, type, context...), definition.resolve(nullptr, type, context...)
      );
      if (!defaultsMatch) {
        std::println(stderr, "{}", type);
      }
      TEST_CHECK(defaultsMatch);
    } catch (const std::exception& e) {
      std::println(stderr, "{}: {}", type, e.what());
      TEST_CHECK(false); // definition threw while validating itself
    }
  }

} // namespace

int main() {
  const BatteryConfig batteryConfig;

  checkDefinition("active_window", activeWindowWidgetDefinition);
  checkDefinition("audio_visualizer", audioVisualizerWidgetDefinition);
  checkDefinition("battery", batteryWidgetDefinition, BatteryWidgetDefinitionContext{.batteryConfig = &batteryConfig});
  checkDefinition("bluetooth", bluetoothWidgetDefinition);
  checkDefinition("brightness", brightnessWidgetDefinition);
  checkDefinition<true>("caffeine", caffeineWidgetDefinition);
  checkDefinition("clipboard", clipboardWidgetDefinition);
  checkDefinition("clock", clockWidgetDefinition);
  checkDefinition("control-center", controlCenterWidgetDefinition);
  checkDefinition("custom_button", customButtonWidgetDefinition);
  checkDefinition("keyboard_layout", keyboardLayoutWidgetDefinition);
  checkDefinition("launcher", launcherWidgetDefinition);
  checkDefinition("lock_keys", lockKeysWidgetDefinition);
  checkDefinition("media", mediaWidgetDefinition);
  checkDefinition("network", networkWidgetDefinition);
  checkDefinition<true>("nightlight", nightlightWidgetDefinition);
  checkDefinition("notifications", notificationWidgetDefinition);
  checkDefinition<true>("power_profile", powerProfileWidgetDefinition);
  checkDefinition("privacy", privacyWidgetDefinition);
  checkDefinition("screenshot", screenshotWidgetDefinition);
  checkDefinition("session", sessionWidgetDefinition);
  checkDefinition("settings", settingsWidgetDefinition);
  checkDefinition("spacer", spacerWidgetDefinition);
  checkDefinition("sysmon", sysmonWidgetDefinition, SysmonWidgetDefinitionContext{});

  WidgetConfig invalidKeyboardLayout;
  invalidKeyboardLayout.type = "keyboard_layout";
  invalidKeyboardLayout.settings["show_glyph"] = false;
  invalidKeyboardLayout.settings["show_label"] = false;
  const auto keyboardError = settings::validateWidgetSemantics("keyboard_layout", &invalidKeyboardLayout);
  TEST_CHECK(keyboardError.has_value() && *keyboardError == "show_glyph and show_label cannot both be false");
  invalidKeyboardLayout.settings["show_label"] = true;
  TEST_CHECK(!settings::validateWidgetSemantics("keyboard_layout", &invalidKeyboardLayout).has_value());

  WidgetConfig invalidSysmon;
  invalidSysmon.type = "sysmon";
  invalidSysmon.settings["show_glyph"] = false;
  invalidSysmon.settings["show_value"] = false;
  invalidSysmon.settings["visualization"] = std::string("none");
  const auto sysmonError = settings::validateWidgetSemantics("sysmon", &invalidSysmon);
  TEST_CHECK(
      sysmonError.has_value()
      && *sysmonError == "show_glyph, custom_label, show_value, and visualization cannot all be disabled"
  );
  invalidSysmon.settings["custom_label"] = std::string("CPU");
  TEST_CHECK(!settings::validateWidgetSemantics("sysmon", &invalidSysmon).has_value());
  invalidSysmon.settings["custom_label"] = std::string();
  invalidSysmon.settings["show_value"] = true;
  TEST_CHECK(!settings::validateWidgetSemantics("sysmon", &invalidSysmon).has_value());

  WidgetConfig networkPrecision;
  networkPrecision.type = "sysmon";
  networkPrecision.settings["stat"] = std::string("net_rx");
  networkPrecision.settings["network_speed_decimal_places"] = std::int64_t{-1};
  TEST_CHECK(
      sysmonWidgetDefinition().resolve(&networkPrecision, "sysmon", {}).networkSpeedDecimalPlaces
      == FormatUnits::kMinCompactByteRateDecimalPlaces
  );
  networkPrecision.settings["network_speed_decimal_places"] = std::int64_t{4};
  TEST_CHECK(
      sysmonWidgetDefinition().resolve(&networkPrecision, "sysmon", {}).networkSpeedDecimalPlaces
      == FormatUnits::kMaxCompactByteRateDecimalPlaces
  );

  Config networkPrecisionConfig;
  networkPrecision.settings["network_speed_compact"] = false;
  networkPrecisionConfig.widgets.emplace("network-precision", networkPrecision);
  const auto networkPrecisionVisible = [&](const std::vector<settings::WidgetSettingSpec>& specs) {
    const auto precision =
        std::ranges::find(specs, "network_speed_decimal_places", [](const settings::WidgetSettingSpec& candidate) {
          return std::string_view(candidate.schema.key);
        });
    TEST_CHECK(precision != specs.end());
    return settings::widgetSettingIsVisible(
        networkPrecisionConfig, "network-precision", *precision, specs, settings::WidgetSettingCapabilities{}
    );
  };
  const auto barSysmonSpecs =
      settings::widgetSettingSpecs("sysmon", &networkPrecisionConfig.widgets.at("network-precision"), "", false);
  TEST_CHECK(!networkPrecisionVisible(barSysmonSpecs));
  networkPrecisionConfig.widgets.at("network-precision").settings["network_speed_compact"] = true;
  TEST_CHECK(networkPrecisionVisible(barSysmonSpecs));
  networkPrecisionConfig.widgets.at("network-precision").settings["stat"] = std::string("cpu_usage");
  TEST_CHECK(!networkPrecisionVisible(barSysmonSpecs));

  const auto desktopSysmonSpecs = desktop_settings::desktopWidgetSettingSpecs("sysmon");
  networkPrecisionConfig.widgets.at("network-precision").settings["stat2"] = std::string("net_tx");
  TEST_CHECK(networkPrecisionVisible(desktopSysmonSpecs));
  networkPrecisionConfig.widgets.at("network-precision").settings["network_speed_compact"] = false;
  TEST_CHECK(!networkPrecisionVisible(desktopSysmonSpecs));

  checkDefinition("taskbar", taskbarWidgetDefinition);
  WidgetConfig spacedTaskbar;
  spacedTaskbar.type = "taskbar";
  spacedTaskbar.settings["item_spacing"] = 3.0;
  spacedTaskbar.settings["icon_scale"] = 0.75;
  const auto resolvedTaskbar = taskbarWidgetDefinition().resolve(&spacedTaskbar, "taskbar");
  TEST_CHECK(resolvedTaskbar.itemSpacing == 3);
  TEST_CHECK(resolvedTaskbar.iconScale == 0.75F);
  TEST_CHECK(resolvedTaskbar.showRunning);
  WidgetConfig pinnedOnlyTaskbar;
  pinnedOnlyTaskbar.type = "taskbar";
  pinnedOnlyTaskbar.settings["show_running"] = false;
  TEST_CHECK(!taskbarWidgetDefinition().resolve(&pinnedOnlyTaskbar, "taskbar").showRunning);

  Config taskbarConfig;
  WidgetConfig taskbar;
  taskbar.type = "taskbar";
  taskbar.settings["group_by_workspace"] = true;
  taskbar.settings["capsule"] = false;
  taskbar.settings["show_window_title"] = true;
  taskbarConfig.widgets.emplace("taskbar-test", std::move(taskbar));
  const auto taskbarSpecs =
      settings::widgetSettingSpecs("taskbar", &taskbarConfig.widgets.at("taskbar-test"), "sans-serif");
  const auto settingVisible = [&](std::string_view key, bool workspaceGrouping) {
    const auto spec = std::ranges::find(taskbarSpecs, key, [](const settings::WidgetSettingSpec& candidate) {
      return std::string_view(candidate.schema.key);
    });
    if (spec == taskbarSpecs.end()) {
      std::println(stderr, "missing presentation spec '{}'", key);
    }
    TEST_CHECK(spec != taskbarSpecs.end());
    return settings::widgetSettingIsVisible(
        taskbarConfig, "taskbar-test", *spec, taskbarSpecs,
        settings::WidgetSettingCapabilities{
            .taskbarWorkspaceGrouping = workspaceGrouping,
        }
    );
  };
  // Unsupported workspace grouping uses capability-gated defaults.
  TEST_CHECK(
      !settingVisible("group_by_workspace", false)
      && settingVisible("pinned", false)
      && settingVisible("show_running", false)
      && settingVisible("show_window_title", false)
      && settingVisible("window_title_max_width", false)
      && !settingVisible("capsule_radius", false)
  );
  taskbarConfig.widgets.at("taskbar-test").settings["capsule"] = true;
  TEST_CHECK(settingVisible("capsule_radius", false));
  taskbarConfig.widgets.at("taskbar-test").settings["capsule"] = false;
  // Supported workspace grouping exposes grouped settings.
  TEST_CHECK(
      settingVisible("group_by_workspace", true)
      && !settingVisible("pinned", true)
      && !settingVisible("show_running", true)
      && settingVisible("capsule_radius", true)
  );
  // Capability visibility must not mutate persisted workspace grouping.
  TEST_CHECK(taskbarConfig.widgets.at("taskbar-test").getBool("group_by_workspace", false));
  checkDefinition<true>("test", testWidgetDefinition);
  checkDefinition("text", textWidgetDefinition);
  checkDefinition<true>("theme_mode", themeModeWidgetDefinition);
  checkDefinition("tray", trayWidgetDefinition, TrayWidgetDefinitionContext{});
  checkDefinition("volume", volumeWidgetDefinition);

  WidgetConfig inputVolume;
  inputVolume.type = "volume";
  inputVolume.settings["device"] = std::string("input");
  inputVolume.tables["effects_profile_glyphs"] = WidgetSettingStringMap{{"Noise Canceling", "microphone"}};
  const auto inputOptions = volumeWidgetDefinition().resolve(&inputVolume, "volume");
  TEST_CHECK(
      inputOptions.device == VolumeWidgetTarget::Input
      && inputOptions.effectsProfileGlyphs
          == std::unordered_map<std::string, std::string>{{"Noise Canceling", "microphone"}}
  );
  const auto inputGestures = noctalia::bar::gestureDefaultsForType("volume", &inputVolume);
  const auto inputMute =
      std::ranges::find(inputGestures, noctalia::bar::Gesture::Right, &noctalia::bar::GestureBinding::gesture);
  TEST_CHECK(inputMute != inputGestures.end() && inputMute->action == "mic-mute");

  Config glyphConfig;
  glyphConfig.widgets.emplace("mic", inputVolume);
  WidgetConfig networkDownload;
  networkDownload.type = "sysmon";
  networkDownload.settings["stat"] = std::string("net_rx");
  glyphConfig.widgets.emplace("network-download", std::move(networkDownload));
  const auto pickerEntries = settings::widgetPickerEntries(glyphConfig);
  const auto pickerGlyph = [&](std::string_view name) -> std::string_view {
    const auto entry = std::ranges::find(pickerEntries, name, &settings::WidgetPickerEntry::value);
    if (entry == pickerEntries.end()) {
      std::println(stderr, "{} is missing from picker entries", name);
    }
    TEST_CHECK(entry != pickerEntries.end());
    return entry->icon;
  };
  TEST_CHECK(pickerGlyph("mic") == "microphone");
  TEST_CHECK(pickerGlyph("network-download") == "download");

  // hide_when_inactive keys off microphone capture, so the settings UI must only offer it on a
  // volume widget bound to the input device.
  WidgetConfig outputVolume;
  outputVolume.type = "volume";
  glyphConfig.widgets.emplace("speaker", std::move(outputVolume));
  const auto hideWhenInactiveVisible = [&](std::string_view widgetName) {
    const WidgetConfig& widget = glyphConfig.widgets.at(std::string(widgetName));
    const auto specs = settings::widgetSettingSpecs("volume", &widget, "", false);
    const auto spec = std::ranges::find(specs, "hide_when_inactive", [](const settings::WidgetSettingSpec& candidate) {
      return std::string_view(candidate.schema.key);
    });
    TEST_CHECK(spec != specs.end());
    return settings::widgetSettingIsVisible(
        glyphConfig, widgetName, *spec, specs, settings::WidgetSettingCapabilities{}
    );
  };
  TEST_CHECK(hideWhenInactiveVisible("mic"));
  TEST_CHECK(!hideWhenInactiveVisible("speaker"));
  // `custom_label` and `show_glyph` are common settings: every built-in widget exposes them,
  // and a typed definition declaring the same key (sysmon, keyboard_layout) shadows the
  // common spec so exactly one control exists per key.
  const auto sysmonSpecs = settings::widgetSettingSpecs("sysmon", nullptr, "sans-serif", false);
  for (const auto& [key, expectedType] :
       {std::pair{"custom_label", noctalia::config::schema::WidgetSettingType::String},
        std::pair{"show_glyph", noctalia::config::schema::WidgetSettingType::Bool}}) {
    for (const std::string_view type : {"battery", "clock", "taskbar", "volume"}) {
      const auto spec = settings::findWidgetSettingSpec(type, key);
      if (!spec.has_value() || spec->schema.type != expectedType) {
        std::println(stderr, "{}: {}", type, key);
      }
      TEST_CHECK(spec.has_value());
      TEST_CHECK(spec->schema.type == expectedType);
    }
    const auto sysmonCount = std::ranges::count_if(sysmonSpecs, [&key](const settings::WidgetSettingSpec& spec) {
      return spec.schema.key == key;
    });
    if (sysmonCount != 1) {
      std::println(stderr, "sysmon: {} spec count is {}", key, sysmonCount);
    }
    TEST_CHECK(sysmonCount == 1);
  }
  WidgetConfig customLabelConfig;
  customLabelConfig.type = "battery";
  customLabelConfig.settings["custom_label"] = std::string("BAT");
  TEST_CHECK(resolveCommonWidgetOptions(BarConfig{}, &customLabelConfig, "battery", 1.0F).customLabel == "BAT");
  WidgetConfig glyphHidden;
  glyphHidden.type = "battery";
  glyphHidden.settings["show_glyph"] = false;
  TEST_CHECK(!resolveCommonWidgetOptions(BarConfig{}, &glyphHidden, "battery", 1.0F).showGlyph);
  checkDefinition("wallpaper", wallpaperWidgetDefinition);
  checkDefinition("weather", weatherWidgetDefinition);

  return 0;
}
