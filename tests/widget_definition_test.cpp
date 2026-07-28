// Every typed widget definition validates itself lazily: `field()` and `WidgetDefinition::validate()`
// throw std::logic_error on the first resolve()/schemaFields() call, and WidgetFactory::create() does
// not catch. Without this test a malformed definition ships as an uncaught exception at bar build.
#include "shell/bar/widgets/active_window_widget_definition.h"
#include "shell/bar/widgets/audio_visualizer_widget_definition.h"
#include "shell/bar/widgets/battery_widget_definition.h"
#include "shell/bar/widgets/bluetooth_widget_definition.h"
#include "shell/bar/widgets/brightness_widget_definition.h"
#include "shell/bar/widgets/clipboard_widget_definition.h"
#include "shell/bar/widgets/clock_widget_definition.h"
#include "shell/bar/widgets/control_center_widget_definition.h"
#include "shell/bar/widgets/custom_button_widget_definition.h"
#include "shell/bar/widgets/launcher_widget_definition.h"
#include "shell/bar/widgets/lock_keys_widget_definition.h"
#include "shell/bar/widgets/media_widget_definition.h"
#include "shell/bar/widgets/network_widget_definition.h"
#include "shell/bar/widgets/notification_widget_definition.h"
#include "shell/bar/widgets/privacy_widget_definition.h"
#include "shell/bar/widgets/screenshot_widget_definition.h"
#include "shell/bar/widgets/session_widget_definition.h"
#include "shell/bar/widgets/settings_widget_definition.h"
#include "shell/bar/widgets/spacer_widget_definition.h"
#include "shell/bar/widgets/sysmon_widget_definition.h"
#include "shell/bar/widgets/text_widget_definition.h"
#include "shell/bar/widgets/tray_widget_definition.h"
#include "shell/bar/widgets/wallpaper_widget_definition.h"
#include "shell/bar/widgets/weather_widget_definition.h"
#include "system/battery_warning_monitor.h"

#include <exception>
#include <format>
#include <print>
#include <set>
#include <string>
#include <string_view>

// The battery definition's finalize hook reaches into the warning-threshold helper, which would drag
// UPower, notifications and i18n into a test about definition well-formedness. Threshold behaviour is
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
  template <typename Accessor, typename... Context>
  void checkDefinition(std::string_view type, Accessor accessor, const Context&... context) {
    try {
      const auto& definition = accessor();
      if (definition.type != type) {
        fail(type, std::format("definition declares the type '{}'", definition.type));
      }

      const auto schema = definition.schemaFields();
      if (schema.empty()) {
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
        config.settings[field.key] = field.defaultValue;
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
  checkDefinition("clipboard", clipboardWidgetDefinition);
  checkDefinition("clock", clockWidgetDefinition);
  checkDefinition("control-center", controlCenterWidgetDefinition);
  checkDefinition("custom_button", customButtonWidgetDefinition);
  checkDefinition("launcher", launcherWidgetDefinition);
  checkDefinition("lock_keys", lockKeysWidgetDefinition);
  checkDefinition("media", mediaWidgetDefinition);
  checkDefinition("network", networkWidgetDefinition);
  checkDefinition("notifications", notificationWidgetDefinition);
  checkDefinition("privacy", privacyWidgetDefinition);
  checkDefinition("screenshot", screenshotWidgetDefinition);
  checkDefinition("session", sessionWidgetDefinition);
  checkDefinition("settings", settingsWidgetDefinition);
  checkDefinition("spacer", spacerWidgetDefinition);
  checkDefinition("sysmon", sysmonWidgetDefinition, SysmonWidgetDefinitionContext{});
  checkDefinition("text", textWidgetDefinition);
  checkDefinition("tray", trayWidgetDefinition, TrayWidgetDefinitionContext{});
  checkDefinition("wallpaper", wallpaperWidgetDefinition);
  checkDefinition("weather", weatherWidgetDefinition);

  return g_ok ? 0 : 1;
}
