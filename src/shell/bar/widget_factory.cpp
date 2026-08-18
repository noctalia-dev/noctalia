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
#include <array>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>
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

  struct BuiltinWidgetContext {
    const std::string& name;
    const WidgetConfig* config;
    wl_output* output;
    float contentScale;
    const std::string& settingContext;
    const std::string& barPosition;
    const std::string& barName;
    float widgetSpacing;
    bool verticalBar;
  };

  using BuiltinWidgetCreator =
      std::unique_ptr<Widget> (*)(const WidgetFactory& factory, const BuiltinWidgetContext& context);
  struct BuiltinWidget {
    std::string_view type;
    BuiltinWidgetCreator create;
  };

  static constexpr auto kBuiltinWidgets = std::to_array<BuiltinWidget>({
      {"active_window", [](const WidgetFactory& f, const BuiltinWidgetContext& context) {
            return createWidget<ActiveWindowWidget>(
                context.contentScale, f.m_configService, f.m_platform,
                activeWindowWidgetDefinition().resolve(context.config, context.settingContext)
            );
          }},
      {"audio_visualizer", [](const WidgetFactory& f, const BuiltinWidgetContext& context) {
            return createWidget<AudioVisualizerWidget>(
                context.contentScale, f.m_audioSpectrum,
                audioVisualizerWidgetDefinition().resolve(context.config, context.settingContext)
            );
          }},
      {"battery", [](const WidgetFactory& f, const BuiltinWidgetContext& context) {
            return createWidget<BatteryWidget>(
                context.contentScale, f.m_upower,
                batteryWidgetDefinition().resolve(
                    context.config, context.settingContext,
                    BatteryWidgetDefinitionContext{
                        .batteryConfig = &f.m_config.battery,
                        .upower = f.m_upower,
                    }
                )
            );
          }},
      {"bluetooth", [](const WidgetFactory& f, const BuiltinWidgetContext& context) {
            return createWidget<BluetoothWidget>(
                context.contentScale, f.m_bluetooth, context.output,
                bluetoothWidgetDefinition().resolve(context.config, context.settingContext)
            );
          }},
      {"brightness", [](const WidgetFactory& f, const BuiltinWidgetContext& context) {
            return createWidget<BrightnessWidget>(
                context.contentScale, f.m_brightness, context.output,
                brightnessWidgetDefinition().resolve(context.config, context.settingContext)
            );
          }},
      {"caffeine", [](const WidgetFactory& f, const BuiltinWidgetContext& context) {
            return createWidget<IdleInhibitorWidget>(context.contentScale, f.m_idleInhibitor);
          }},
      {"clipboard", [](const WidgetFactory& f, const BuiltinWidgetContext& context) {
            if (!f.m_config.shell.clipboardEnabled) {
              return std::unique_ptr<Widget>{};
            }
            return createWidget<ClipboardWidget>(
                context.contentScale, context.output,
                clipboardWidgetDefinition().resolve(context.config, context.settingContext)
            );
          }},
      {"clock", [](const WidgetFactory&, const BuiltinWidgetContext& context) {
            return createWidget<ClockWidget>(
                context.contentScale, context.output,
                clockWidgetDefinition().resolve(context.config, context.settingContext)
            );
          }},
      {"control-center", [](const WidgetFactory&, const BuiltinWidgetContext& context) {
            return createWidget<ControlCenterWidget>(
                context.contentScale, context.output,
                controlCenterWidgetDefinition().resolve(context.config, context.settingContext)
            );
          }},
      {"custom_button", [](const WidgetFactory&, const BuiltinWidgetContext& context) {
            return createWidget<CustomButtonWidget>(
                context.contentScale, customButtonWidgetDefinition().resolve(context.config, context.settingContext)
            );
          }},
#ifndef NDEBUG
      {"debug_indicator", [](const WidgetFactory&, const BuiltinWidgetContext& context) {
            return createWidget<DebugIndicatorWidget>(context.contentScale);
          }},
#endif
      {"keyboard_layout", [](const WidgetFactory& f, const BuiltinWidgetContext& context) {
            return createWidget<KeyboardLayoutWidget>(
                context.contentScale, f.m_platform,
                keyboardLayoutWidgetDefinition().resolve(context.config, context.settingContext),
                f.m_config.shell.keyboardLayout.customLabels
            );
          }},
      {"launcher", [](const WidgetFactory&, const BuiltinWidgetContext& context) {
            return createWidget<LauncherWidget>(
                context.contentScale, context.output,
                launcherWidgetDefinition().resolve(context.config, context.settingContext)
            );
          }},
      {"lock_keys", [](const WidgetFactory& f, const BuiltinWidgetContext& context) {
            if (f.m_lockKeys == nullptr) {
              return std::unique_ptr<Widget>{};
            }
            return createWidget<LockKeysWidget>(
                context.contentScale, f.m_lockKeys,
                lockKeysWidgetDefinition().resolve(context.config, context.settingContext)
            );
          }},
      {"media", [](const WidgetFactory& f, const BuiltinWidgetContext& context) {
            return createWidget<MediaWidget>(
                context.contentScale, f.m_mpris, f.m_httpClient, context.output,
                mediaWidgetDefinition().resolve(context.config, context.settingContext)
            );
          }},
      {"network", [](const WidgetFactory& f, const BuiltinWidgetContext& context) {
            return createWidget<NetworkWidget>(
                context.contentScale, f.m_network, f.m_externalIp, f.m_sysmon, context.output,
                networkWidgetDefinition().resolve(context.config, context.settingContext)
            );
          }},
      {"nightlight", [](const WidgetFactory& f, const BuiltinWidgetContext& context) {
            return createWidget<NightLightWidget>(context.contentScale, f.m_nightLight);
          }},
      {"notifications", [](const WidgetFactory& f, const BuiltinWidgetContext& context) {
            return createWidget<NotificationWidget>(
                context.contentScale, f.m_notifications, context.output,
                notificationWidgetDefinition().resolve(context.config, context.settingContext)
            );
          }},
      {"power_profile", [](const WidgetFactory& f, const BuiltinWidgetContext& context) {
            return createWidget<PowerProfileWidget>(context.contentScale, f.m_powerProfiles);
          }},
      {"privacy", [](const WidgetFactory& f, const BuiltinWidgetContext& context) {
            return createWidget<PrivacyWidget>(
                context.contentScale, f.m_audio, &f.m_configService,
                privacyWidgetDefinition().resolve(context.config, context.settingContext)
            );
          }},
      {"screenshot", [](const WidgetFactory& f, const BuiltinWidgetContext& context) {
            if (f.m_screenshots == nullptr || f.m_renderContext == nullptr
                || !f.m_screenshots->available()) {
              return std::unique_ptr<Widget>{};
            }
            return createWidget<ScreenshotWidget>(
                context.contentScale, context.output, *f.m_screenshots, f.m_configService,
                f.m_platform, *f.m_renderContext, context.barPosition,
                screenshotWidgetDefinition().resolve(context.config, context.settingContext)
            );
          }},
      {"session", [](const WidgetFactory&, const BuiltinWidgetContext& context) {
            return createWidget<SessionWidget>(
                context.contentScale, context.output,
                sessionWidgetDefinition().resolve(context.config, context.settingContext)
            );
          }},
      {"settings", [](const WidgetFactory&, const BuiltinWidgetContext& context) {
            return createWidget<SettingsWidget>(
                context.contentScale, context.output,
                settingsWidgetDefinition().resolve(context.config, context.settingContext)
            );
          }},
      {"spacer", [](const WidgetFactory&, const BuiltinWidgetContext& context) {
            return createWidget<SpacerWidget>(
                context.contentScale, context.verticalBar,
                spacerWidgetDefinition().resolve(context.config, context.settingContext)
            );
          }},
      {"sysmon", [](const WidgetFactory& f, const BuiltinWidgetContext& context) {
            return createWidget<SysmonWidget>(
                context.contentScale, f.m_sysmon, f.m_configService,
                sysmonWidgetDefinition().resolve(
                    context.config, context.settingContext,
                    SysmonWidgetDefinitionContext{.verticalBar = context.verticalBar}
                )
            );
          }},
      {"taskbar", [](const WidgetFactory& f, const BuiltinWidgetContext& context) {
            return createWidget<TaskbarWidget>(
                context.contentScale, f.m_platform, f.m_configService, context.output,
                taskbarWidgetDefinition().resolve(context.config, context.settingContext),
                TaskbarWidgetContext{
                    .barPosition = context.barPosition,
                    .barName = context.barName,
                    .widgetName = context.name,
                }
            );
          }},
      {"test", [](const WidgetFactory&, const BuiltinWidgetContext& context) {
            return createWidget<TestWidget>(context.contentScale, context.output);
          }},
      {"text", [](const WidgetFactory&, const BuiltinWidgetContext& context) {
            return createWidget<TextWidget>(
                context.contentScale, textWidgetDefinition().resolve(context.config, context.settingContext)
            );
          }},
      {"theme_mode", [](const WidgetFactory& f, const BuiltinWidgetContext& context) {
            return createWidget<ThemeModeWidget>(context.contentScale, f.m_themeService);
          }},
      {"tray", [](const WidgetFactory& f, const BuiltinWidgetContext& context) {
            return createWidget<TrayWidget>(
                context.contentScale, f.m_configService, f.m_tray,
                trayWidgetDefinition().resolve(
                    context.config, context.settingContext,
                    TrayWidgetDefinitionContext{
                        .barPosition = context.barPosition,
                        .inlineEntryGap = context.widgetSpacing,
                    }
                )
            );
          }},
      {"volume", [](const WidgetFactory& f, const BuiltinWidgetContext& context) {
            return createWidget<VolumeWidget>(
                context.contentScale, f.m_audio, f.m_easyEffects,
                volumeWidgetDefinition().resolve(context.config, context.settingContext)
            );
          }},
      {"wallpaper", [](const WidgetFactory&, const BuiltinWidgetContext& context) {
            return createWidget<WallpaperWidget>(
                context.contentScale, context.output,
                wallpaperWidgetDefinition().resolve(context.config, context.settingContext)
            );
          }},
      {"weather", [](const WidgetFactory& f, const BuiltinWidgetContext& context) {
            return createWidget<WeatherWidget>(
                context.contentScale, f.m_weather, context.output,
                weatherWidgetDefinition().resolve(context.config, context.settingContext)
            );
          }},
      {"workspaces", [](const WidgetFactory& f, const BuiltinWidgetContext& context) {
            return createWidget<WorkspacesWidget>(
                context.contentScale, f.m_platform, f.m_configService, context.output,
                workspacesWidgetDefinition().resolve(context.config, context.settingContext)
            );
          }},
  });

  const BuiltinWidgetContext context{
      .name = name,
      .config = wc,
      .output = output,
      .contentScale = contentScale,
      .settingContext = settingContext,
      .barPosition = barPosition,
      .barName = barName,
      .widgetSpacing = widgetSpacing,
      .verticalBar = barPosition == "left" || barPosition == "right",
  };
  if (const auto builtin = std::ranges::find(kBuiltinWidgets, type, &BuiltinWidget::type);
      builtin != kBuiltinWidgets.end()) {
    return builtin->create(*this, context);
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
            .pluginDir = pluginEntry->pluginDir,
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

  kLog.warn("widget factory: unknown widget \"{}\"", name);
  return nullptr;
}
