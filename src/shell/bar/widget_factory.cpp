#include "shell/bar/widget_factory.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/log.h"
#include "shell/bar/widgets/active_window_widget.h"
#include "shell/bar/widgets/active_window_widget_definition.h"
#include "shell/bar/widgets/audio_visualizer_widget.h"
#include "shell/bar/widgets/audio_visualizer_widget_definition.h"
#include "shell/bar/widgets/battery_widget.h"
#include "shell/bar/widgets/battery_widget_definition.h"
#include "shell/bar/widgets/bluetooth_widget.h"
#include "shell/bar/widgets/bluetooth_widget_definition.h"
#include "shell/bar/widgets/brightness_widget.h"
#include "shell/bar/widgets/brightness_widget_definition.h"
#include "shell/bar/widgets/clipboard_widget.h"
#include "shell/bar/widgets/clipboard_widget_definition.h"
#include "shell/bar/widgets/clock_widget.h"
#include "shell/bar/widgets/clock_widget_definition.h"
#include "shell/bar/widgets/control_center_widget.h"
#include "shell/bar/widgets/control_center_widget_definition.h"
#include "shell/bar/widgets/custom_button_widget.h"
#include "shell/bar/widgets/custom_button_widget_definition.h"
#ifndef NDEBUG
#include "shell/bar/widgets/debug_indicator_widget.h"
#endif
#include "capture/screenshot_service.h"
#include "scripting/plugin_manifest.h"
#include "scripting/plugin_registry.h"
#include "shell/bar/widgets/idle_inhibitor_widget.h"
#include "shell/bar/widgets/keyboard_layout_widget.h"
#include "shell/bar/widgets/keyboard_layout_widget_definition.h"
#include "shell/bar/widgets/launcher_widget.h"
#include "shell/bar/widgets/launcher_widget_definition.h"
#include "shell/bar/widgets/lock_keys_widget.h"
#include "shell/bar/widgets/lock_keys_widget_definition.h"
#include "shell/bar/widgets/media_widget.h"
#include "shell/bar/widgets/media_widget_definition.h"
#include "shell/bar/widgets/network_widget.h"
#include "shell/bar/widgets/network_widget_definition.h"
#include "shell/bar/widgets/nightlight_widget.h"
#include "shell/bar/widgets/notification_widget.h"
#include "shell/bar/widgets/notification_widget_definition.h"
#include "shell/bar/widgets/plugin_widget.h"
#include "shell/bar/widgets/power_profile_widget.h"
#include "shell/bar/widgets/privacy_widget.h"
#include "shell/bar/widgets/privacy_widget_definition.h"
#include "shell/bar/widgets/screenshot_widget.h"
#include "shell/bar/widgets/screenshot_widget_definition.h"
#include "shell/bar/widgets/session_widget.h"
#include "shell/bar/widgets/session_widget_definition.h"
#include "shell/bar/widgets/settings_widget.h"
#include "shell/bar/widgets/settings_widget_definition.h"
#include "shell/bar/widgets/spacer_widget.h"
#include "shell/bar/widgets/spacer_widget_definition.h"
#include "shell/bar/widgets/sysmon_widget.h"
#include "shell/bar/widgets/sysmon_widget_definition.h"
#include "shell/bar/widgets/taskbar_widget.h"
#include "shell/bar/widgets/taskbar_widget_definition.h"
#include "shell/bar/widgets/test_widget.h"
#include "shell/bar/widgets/text_widget.h"
#include "shell/bar/widgets/text_widget_definition.h"
#include "shell/bar/widgets/theme_mode_widget.h"
#include "shell/bar/widgets/tray_widget.h"
#include "shell/bar/widgets/tray_widget_definition.h"
#include "shell/bar/widgets/volume_widget.h"
#include "shell/bar/widgets/volume_widget_definition.h"
#include "shell/bar/widgets/wallpaper_widget.h"
#include "shell/bar/widgets/wallpaper_widget_definition.h"
#include "shell/bar/widgets/weather_widget.h"
#include "shell/bar/widgets/weather_widget_definition.h"
#include "shell/bar/widgets/workspaces_widget.h"
#include "shell/bar/widgets/workspaces_widget_definition.h"
#include "ui/style.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace {
  constexpr Logger kLog("shell");

  template <typename T, typename... Args> std::unique_ptr<Widget> createWidget(float contentScale, Args&&... args) {
    auto widget = std::make_unique<T>(std::forward<Args>(args)...);
    widget->setContentScale(contentScale);
    return widget;
  }

} // namespace

WidgetFactory::WidgetFactory(const BarServices& services)
    : m_platform(services.platform), m_configService(services.config), m_config(services.config.config()),
      m_notifications(services.notifications), m_tray(services.tray), m_audio(services.audio),
      m_easyEffects(services.easyEffects), m_upower(services.upower), m_sysmon(services.sysmon),
      m_powerProfiles(services.powerProfiles), m_network(services.network), m_externalIp(services.externalIp),
      m_idleInhibitor(services.idleInhibitor), m_mpris(services.mpris), m_audioSpectrum(services.audioSpectrum),
      m_httpClient(services.httpClient), m_weather(services.weather), m_nightLight(services.nightLight),
      m_themeService(services.theme), m_bluetooth(services.bluetooth), m_brightness(services.brightness),
      m_lockKeys(services.lockKeys), m_clipboard(services.clipboard), m_fileWatcher(services.fileWatcher),
      m_screenshots(services.screenshots), m_renderContext(services.renderContext), m_scriptApi(services.scriptApi) {
  scripting::PluginRegistry::instance().ensureScanned();
}

WidgetFactory::~WidgetFactory() = default;

std::unique_ptr<Widget> WidgetFactory::create(
    const std::string& name, wl_output* output, float contentScale, const std::string& barPosition,
    const std::string& barName, float widgetSpacing, bool enableScroll
) const {
  // Resolve: if name matches a [widget.<name>] entry, use its type + settings.
  // Otherwise treat the name itself as the widget type with default settings.
  const WidgetConfig* wc = nullptr;
  std::string type = name;

  auto it = m_config.widgets.find(name);
  if (it != m_config.widgets.end()) {
    wc = &it->second;
    type = it->second.type;
  }

  // Config path prefix used when a widget definition reports a bad setting value.
  const std::string settingContext = std::format("widget.{}", name);

  if (type == "active_window") {
    return createWidget<ActiveWindowWidget>(
        contentScale, m_configService, m_platform, activeWindowWidgetDefinition().resolve(wc, settingContext)
    );
  }

  if (type == "audio_visualizer") {
    return createWidget<AudioVisualizerWidget>(
        contentScale, m_audioSpectrum, audioVisualizerWidgetDefinition().resolve(wc, settingContext)
    );
  }

  if (type == "battery") {
    return createWidget<BatteryWidget>(
        contentScale, m_upower,
        batteryWidgetDefinition().resolve(
            wc, settingContext, BatteryWidgetDefinitionContext{.batteryConfig = &m_config.battery, .upower = m_upower}
        )
    );
  }

  if (type == "bluetooth") {
    return createWidget<BluetoothWidget>(
        contentScale, m_bluetooth, output, bluetoothWidgetDefinition().resolve(wc, settingContext)
    );
  }

  if (type == "brightness") {
    return createWidget<BrightnessWidget>(
        contentScale, m_brightness, output, brightnessWidgetDefinition().resolve(wc, settingContext)
    );
  }

  if (type == "clock") {
    return createWidget<ClockWidget>(contentScale, output, clockWidgetDefinition().resolve(wc, settingContext));
  }

  if (type == "clipboard") {
    if (!m_config.shell.clipboardEnabled) {
      return nullptr;
    }
    return createWidget<ClipboardWidget>(contentScale, output, clipboardWidgetDefinition().resolve(wc, settingContext));
  }

  if (type == "control-center") {
    return createWidget<ControlCenterWidget>(
        contentScale, output, controlCenterWidgetDefinition().resolve(wc, settingContext)
    );
  }

  if (type == "custom_button") {
    return createWidget<CustomButtonWidget>(contentScale, customButtonWidgetDefinition().resolve(wc, settingContext));
  }

  if (type == "caffeine") {
    return createWidget<IdleInhibitorWidget>(contentScale, m_idleInhibitor);
  }

  if (type == "keyboard_layout") {
    return createWidget<KeyboardLayoutWidget>(
        contentScale, m_platform, keyboardLayoutWidgetDefinition().resolve(wc, settingContext),
        m_config.shell.keyboardLayout.customLabels
    );
  }

  if (type == "launcher") {
    return createWidget<LauncherWidget>(contentScale, output, launcherWidgetDefinition().resolve(wc, settingContext));
  }

  if (type == "lock_keys") {
    if (m_lockKeys == nullptr) {
      return nullptr;
    }
    return createWidget<LockKeysWidget>(
        contentScale, m_lockKeys, lockKeysWidgetDefinition().resolve(wc, settingContext)
    );
  }

  if (type == "media") {
    return createWidget<MediaWidget>(
        contentScale, m_mpris, m_httpClient, output, mediaWidgetDefinition().resolve(wc, settingContext)
    );
  }

  if (type == "network") {
    return createWidget<NetworkWidget>(
        contentScale, m_network, m_externalIp, m_sysmon, output, networkWidgetDefinition().resolve(wc, settingContext)
    );
  }

  if (type == "nightlight") {
    return createWidget<NightLightWidget>(contentScale, m_nightLight);
  }

  if (type == "notifications") {
    return createWidget<NotificationWidget>(
        contentScale, m_notifications, output, notificationWidgetDefinition().resolve(wc, settingContext)
    );
  }

  if (type == "power_profile") {
    return createWidget<PowerProfileWidget>(contentScale, m_powerProfiles);
  }

  if (type == "privacy") {
    return createWidget<PrivacyWidget>(
        contentScale, m_audio, &m_configService, privacyWidgetDefinition().resolve(wc, settingContext)
    );
  }

  if (auto pluginEntry = scripting::PluginRegistry::instance().resolve(type);
      pluginEntry.has_value() && pluginEntry->entry->kind == scripting::PluginEntryKind::Widget) {
    if (m_scriptApi == nullptr) {
      return nullptr;
    }
    const auto* outputInfo = m_platform.findOutputByWl(output);
    const std::string outputName = outputInfo != nullptr ? outputInfo->connectorName : std::string{};
    std::unordered_map<std::string, WidgetSettingValue> overrides;
    if (wc != nullptr) {
      overrides = wc->settings;
      for (const auto& field : pluginEntry->entry->settings) {
        if (field.type != scripting::ManifestFieldType::StringMap) {
          continue;
        }
        if (const auto tableIt = wc->tables.find(field.key); tableIt != wc->tables.end()) {
          overrides.insert_or_assign(field.key, tableIt->second);
        }
      }
    }
    auto seeded = scripting::seedEntrySettings(*pluginEntry->entry, overrides);
    const auto& pluginSettings = m_config.plugins.pluginSettings;
    const auto psIt = pluginSettings.find(pluginEntry->manifest->id);
    static const std::unordered_map<std::string, WidgetSettingValue> kNoPluginOverrides;
    scripting::mergePluginSettings(
        *pluginEntry->manifest, psIt != pluginSettings.end() ? psIt->second : kNoPluginOverrides, seeded
    );
    auto widget = std::make_unique<PluginWidget>(
        scripting::PluginRuntimeContext{
            .entryId = pluginEntry->fullId(),
            .sourcePath = pluginEntry->sourcePath,
            .settings = std::move(seeded),
            .scriptApi = *m_scriptApi,
            .fileWatcher = m_fileWatcher,
            .httpClient = m_httpClient,
            .clipboard = m_clipboard,
            .platform = &m_platform,
            .audioSpectrum = m_audioSpectrum,
            .mpris = m_mpris,
        },
        barName, outputName, enableScroll
    );
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "screenshot") {
    if (m_screenshots == nullptr || m_renderContext == nullptr || !m_screenshots->available()) {
      return nullptr;
    }
    return createWidget<ScreenshotWidget>(
        contentScale, output, *m_screenshots, m_configService, m_platform, *m_renderContext, barPosition,
        screenshotWidgetDefinition().resolve(wc, settingContext)
    );
  }

  if (type == "session") {
    return createWidget<SessionWidget>(contentScale, output, sessionWidgetDefinition().resolve(wc, settingContext));
  }

  if (type == "settings") {
    return createWidget<SettingsWidget>(contentScale, output, settingsWidgetDefinition().resolve(wc, settingContext));
  }

  if (type == "spacer") {
    const bool verticalBar = barPosition == "left" || barPosition == "right";
    return createWidget<SpacerWidget>(contentScale, verticalBar, spacerWidgetDefinition().resolve(wc, settingContext));
  }

  if (type == "text") {
    return createWidget<TextWidget>(contentScale, textWidgetDefinition().resolve(wc, settingContext));
  }

  if (type == "sysmon") {
    const bool verticalBar = barPosition == "left" || barPosition == "right";
    return createWidget<SysmonWidget>(
        contentScale, m_sysmon, m_configService,
        sysmonWidgetDefinition().resolve(wc, settingContext, SysmonWidgetDefinitionContext{.verticalBar = verticalBar})
    );
  }

  if (type == "test") {
    return createWidget<TestWidget>(contentScale, output);
  }

  if (type == "taskbar") {
    return createWidget<TaskbarWidget>(
        contentScale, m_platform, m_configService, output, taskbarWidgetDefinition().resolve(wc, settingContext),
        TaskbarWidgetContext{
            .barPosition = barPosition,
            .barName = barName,
            .widgetName = name,
        }
    );
  }

  if (type == "theme_mode") {
    return createWidget<ThemeModeWidget>(contentScale, m_themeService);
  }

  if (type == "tray") {
    return createWidget<TrayWidget>(
        contentScale, m_configService, m_tray,
        trayWidgetDefinition().resolve(
            wc, settingContext,
            TrayWidgetDefinitionContext{
                .barPosition = barPosition,
                .inlineEntryGap = widgetSpacing,
            }
        )
    );
  }

  if (type == "volume") {
    return createWidget<VolumeWidget>(
        contentScale, m_audio, m_easyEffects, volumeWidgetDefinition().resolve(wc, settingContext)
    );
  }

  if (type == "wallpaper") {
    return createWidget<WallpaperWidget>(contentScale, output, wallpaperWidgetDefinition().resolve(wc, settingContext));
  }

  if (type == "weather") {
    return createWidget<WeatherWidget>(
        contentScale, m_weather, output, weatherWidgetDefinition().resolve(wc, settingContext)
    );
  }

  if (type == "workspaces") {
    return createWidget<WorkspacesWidget>(
        contentScale, m_platform, m_configService, output, workspacesWidgetDefinition().resolve(wc, settingContext)
    );
  }

#ifndef NDEBUG
  if (type == "debug_indicator") {
    auto widget = std::make_unique<DebugIndicatorWidget>();
    widget->setContentScale(contentScale);
    return widget;
  }
#endif

  kLog.warn("widget factory: unknown widget \"{}\"", name);
  return nullptr;
}
