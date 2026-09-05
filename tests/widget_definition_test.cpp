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
#include "shell/settings/widget_settings_registry.h"
#include "system/battery_warning_monitor.h"

#include <algorithm>
#include <exception>
#include <format>
#include <print>
#include <set>
#include <string>
#include <string_view>

// The battery definition's finalize hook reaches into the warning-threshold helper, which would drag
// UPower, notifications and i18n into a test about definition well-formedness. Threshold behavior is
// not under test here, so stand the helper in rather than link that tree.
int batteryWarningThresholdForSelector(
    const BatteryConfig& /*config*/, const UPowerService* /*upower*/, std::string_view /*selector*/
) {
  return 0;
}

namespace {

  bool g_ok = true;

  void fail(std::string_view type, std::string_view message) {
    std::println(stderr, "widget_definition_test: FAIL: {}: {}", type, message);
    g_ok = false;
  }

  // Exercises everything the settings registry and the widget factory ask of a definition: schema
  // projection, presentation projection, and resolution of both an absent config and one that spells
  // out every schema default. `type` is the string WidgetFactory::create() dispatches on, so a
  // definition that disagrees with it would silently never surface its settings.
  template <bool AllowEmpty = false, typename Accessor, typename... Context>
  void checkDefinition(std::string_view type, Accessor accessor, const Context&... context) {
    try {
      const auto& definition = accessor();
      if (definition.type != type) {
        fail(type, std::format("definition declares the type '{}'", definition.type));
      }

      const auto schema = definition.schemaFields();
      if (schema.empty() && !AllowEmpty) {
        fail(type, "definition declares no fields");
        return;
      }

      std::set<std::string> keys;
      for (const auto& field : schema) {
        if (field.key.empty()) {
          fail(type, "field has an empty key");
        }
        if (!keys.insert(field.key).second) {
          fail(type, std::format("duplicate field key '{}'", field.key));
        }
      }

      for (const auto& spec : definition.presentedSettingSpecs()) {
        if (spec.labelKey.empty() || spec.descriptionKey.empty()) {
          fail(type, std::format("field '{}' has no translation keys", spec.schema.key));
        }
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
      if (!definition.fieldValuesEqual(
              definition.resolve(&config, type, context...), definition.resolve(nullptr, type, context...)
          )) {
        fail(type, "resolving the schema defaults does not match the declared field defaults");
      }
    } catch (const std::exception& e) {
      fail(type, e.what());
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
  if (!keyboardError.has_value() || *keyboardError != "show_glyph and show_label cannot both be false") {
    fail("keyboard_layout", "invalid resolved options did not produce the semantic error");
  }
  invalidKeyboardLayout.settings["show_label"] = true;
  if (settings::validateWidgetSemantics("keyboard_layout", &invalidKeyboardLayout).has_value()) {
    fail("keyboard_layout", "valid resolved options produced a semantic error");
  }

  WidgetConfig invalidSysmon;
  invalidSysmon.type = "sysmon";
  invalidSysmon.settings["show_glyph"] = false;
  invalidSysmon.settings["show_value"] = false;
  invalidSysmon.settings["visualization"] = std::string("none");
  const auto sysmonError = settings::validateWidgetSemantics("sysmon", &invalidSysmon);
  if (!sysmonError.has_value() || *sysmonError != "show_glyph, show_value, and visualization cannot all be disabled") {
    fail("sysmon", "invalid resolved options did not produce the semantic error");
  }
  invalidSysmon.settings["show_value"] = true;
  if (settings::validateWidgetSemantics("sysmon", &invalidSysmon).has_value()) {
    fail("sysmon", "valid resolved options produced a semantic error");
  }
  checkDefinition("taskbar", taskbarWidgetDefinition);
  WidgetConfig spacedTaskbar;
  spacedTaskbar.type = "taskbar";
  spacedTaskbar.settings["item_spacing"] = 3.0;
  spacedTaskbar.settings["icon_scale"] = 0.75;
  const auto resolvedTaskbar = taskbarWidgetDefinition().resolve(&spacedTaskbar, "taskbar");
  if (resolvedTaskbar.itemSpacing != 3) {
    fail("taskbar", "item spacing override did not resolve");
  }
  if (resolvedTaskbar.iconScale != 0.75F) {
    fail("taskbar", "icon scale override did not resolve");
  }

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
      fail("taskbar", std::format("missing presentation spec '{}'", key));
      return false;
    }
    return settings::widgetSettingIsVisible(
        taskbarConfig, "taskbar-test", *spec, taskbarSpecs,
        settings::WidgetSettingCapabilities{
            .taskbarWorkspaceGrouping = workspaceGrouping,
        }
    );
  };
  if (settingVisible("group_by_workspace", false)
      || !settingVisible("pinned", false)
      || !settingVisible("show_window_title", false)
      || !settingVisible("window_title_max_width", false)
      || settingVisible("capsule_radius", false)) {
    fail("taskbar", "unsupported workspace grouping did not use capability-gated defaults");
  }
  taskbarConfig.widgets.at("taskbar-test").settings["capsule"] = true;
  if (!settingVisible("capsule_radius", false)) {
    fail("taskbar", "unsupported workspace grouping hid the ordinary capsule radius");
  }
  taskbarConfig.widgets.at("taskbar-test").settings["capsule"] = false;
  if (!settingVisible("group_by_workspace", true)
      || settingVisible("pinned", true)
      || !settingVisible("capsule_radius", true)) {
    fail("taskbar", "supported workspace grouping did not expose grouped settings");
  }
  if (!taskbarConfig.widgets.at("taskbar-test").getBool("group_by_workspace", false)) {
    fail("taskbar", "capability visibility mutated persisted workspace grouping");
  }
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
  if (inputOptions.device != VolumeWidgetTarget::Input
      || inputOptions.effectsProfileGlyphs
          != std::unordered_map<std::string, std::string>{{"Noise Canceling", "microphone"}}) {
    fail("volume", "input device or effects profile glyph map did not resolve");
  }
  const auto inputGestures = noctalia::bar::gestureDefaultsForType("volume", &inputVolume);
  const auto inputMute =
      std::ranges::find(inputGestures, noctalia::bar::Gesture::Right, &noctalia::bar::GestureBinding::gesture);
  if (inputMute == inputGestures.end() || inputMute->action != "mic-mute") {
    fail("volume", "input device did not select microphone gesture defaults");
  }

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
      fail(name, "named widget is missing from picker entries");
      return {};
    }
    return entry->icon;
  };
  if (pickerGlyph("mic") != "microphone") {
    fail("volume", "input device did not select the microphone picker glyph");
  }
  if (pickerGlyph("network-download") != "download") {
    fail("sysmon", "network receive stat did not select the download picker glyph");
  }

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
    if (spec == specs.end()) {
      fail("volume", "hide_when_inactive is missing from the settings specs");
      return false;
    }
    return settings::widgetSettingIsVisible(
        glyphConfig, widgetName, *spec, specs, settings::WidgetSettingCapabilities{}
    );
  };
  if (!hideWhenInactiveVisible("mic")) {
    fail("volume", "hide_when_inactive is hidden on an input widget");
  }
  if (hideWhenInactiveVisible("speaker")) {
    fail("volume", "hide_when_inactive is offered on an output widget");
  }
  checkDefinition("wallpaper", wallpaperWidgetDefinition);
  checkDefinition("weather", weatherWidgetDefinition);

  return g_ok ? 0 : 1;
}
