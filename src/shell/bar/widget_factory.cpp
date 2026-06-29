#include "shell/bar/widget_factory.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/log.h"
#include "shell/bar/widgets/active_window_widget.h"
#include "shell/bar/widgets/audio_visualizer_widget.h"
#include "shell/bar/widgets/battery_widget.h"
#include "shell/bar/widgets/bluetooth_widget.h"
#include "shell/bar/widgets/brightness_widget.h"
#include "shell/bar/widgets/clipboard_widget.h"
#include "shell/bar/widgets/clock_widget.h"
#include "shell/bar/widgets/control_center_widget.h"
#include "shell/bar/widgets/custom_button_widget.h"
#include "system/battery_warning_monitor.h"
#ifndef NDEBUG
#include "shell/bar/widgets/debug_indicator_widget.h"
#endif
#include "capture/screenshot_service.h"
#include "scripting/plugin_manifest.h"
#include "scripting/plugin_registry.h"
#include "shell/bar/widgets/idle_inhibitor_widget.h"
#include "shell/bar/widgets/keyboard_layout_widget.h"
#include "shell/bar/widgets/launcher_widget.h"
#include "shell/bar/widgets/lock_keys_widget.h"
#include "shell/bar/widgets/media_widget.h"
#include "shell/bar/widgets/network_widget.h"
#include "shell/bar/widgets/nightlight_widget.h"
#include "shell/bar/widgets/notification_widget.h"
#include "shell/bar/widgets/plugin_widget.h"
#include "shell/bar/widgets/power_profile_widget.h"
#include "shell/bar/widgets/privacy_widget.h"
#include "shell/bar/widgets/screenshot_widget.h"
#include "shell/bar/widgets/session_widget.h"
#include "shell/bar/widgets/settings_widget.h"
#include "shell/bar/widgets/spacer_widget.h"
#include "shell/bar/widgets/sysmon_widget.h"
#include "shell/bar/widgets/taskbar_widget.h"
#include "shell/bar/widgets/test_widget.h"
#include "shell/bar/widgets/theme_mode_widget.h"
#include "shell/bar/widgets/tray_widget.h"
#include "shell/bar/widgets/volume_widget.h"
#include "shell/bar/widgets/wallpaper_widget.h"
#include "shell/bar/widgets/weather_widget.h"
#include "shell/bar/widgets/workspaces_widget.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace {
  constexpr Logger kLog("shell");

  ActiveWindowTitleScrollMode parseActiveWindowTitleScrollMode(std::string_view value) {
    if (value == "always") {
      return ActiveWindowTitleScrollMode::Always;
    }
    if (value == "on_hover" || value == "hover") {
      return ActiveWindowTitleScrollMode::OnHover;
    }
    return ActiveWindowTitleScrollMode::None;
  }

  ActiveWindowDisplayMode parseActiveWindowDisplayMode(std::string_view value) {
    if (value == "icon_only") {
      return ActiveWindowDisplayMode::IconOnly;
    }
    if (value == "text_only") {
      return ActiveWindowDisplayMode::TextOnly;
    }
    return ActiveWindowDisplayMode::IconAndText;
  }

  MediaTitleScrollMode parseMediaTitleScrollMode(std::string_view value) {
    if (value == "always") {
      return MediaTitleScrollMode::Always;
    }
    if (value == "on_hover" || value == "hover") {
      return MediaTitleScrollMode::OnHover;
    }
    return MediaTitleScrollMode::None;
  }

} // namespace

WidgetFactory::WidgetFactory(const BarServices& services)
    : m_platform(services.platform), m_configService(services.config), m_config(services.config.config()),
      m_notifications(services.notifications), m_tray(services.tray), m_audio(services.audio),
      m_easyEffects(services.easyEffects), m_upower(services.upower), m_sysmon(services.sysmon),
      m_powerProfiles(services.powerProfiles), m_network(services.network), m_idleInhibitor(services.idleInhibitor),
      m_mpris(services.mpris), m_audioSpectrum(services.audioSpectrum), m_httpClient(services.httpClient),
      m_weather(services.weather), m_nightLight(services.nightLight), m_themeService(services.theme),
      m_bluetooth(services.bluetooth), m_brightness(services.brightness), m_lockKeys(services.lockKeys),
      m_clipboard(services.clipboard), m_fileWatcher(services.fileWatcher), m_screenshots(services.screenshots),
      m_renderContext(services.renderContext), m_scriptApi(services.scriptApi) {
  scripting::PluginRegistry::instance().ensureScanned();
}

WidgetFactory::~WidgetFactory() = default;

std::unique_ptr<Widget> WidgetFactory::create(
    const std::string& name, wl_output* output, float contentScale, const std::string& barPosition,
    const std::string& barName, float widgetSpacing
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

  if (type == "active_window") {
    const float maxWidth = static_cast<float>(wc != nullptr ? wc->getDouble("max_length", 260.0) : 260.0);
    const float minWidth = static_cast<float>(wc != nullptr ? wc->getDouble("min_length", 80.0) : 80.0);
    const float iconSize =
        static_cast<float>(wc != nullptr ? wc->getDouble("icon_size", Style::fontSizeBody) : Style::fontSizeBody);
    const std::string titleScroll = wc != nullptr ? wc->getString("title_scroll", "none") : std::string("none");
    const std::string displayMode =
        wc != nullptr ? wc->getString("display", "icon_and_text") : std::string("icon_and_text");
    const bool showEmptyLabel = wc != nullptr ? wc->getBool("show_empty_label", false) : false;
    auto widget = std::make_unique<ActiveWindowWidget>(
        m_configService, m_platform, maxWidth, minWidth, iconSize, parseActiveWindowTitleScrollMode(titleScroll),
        parseActiveWindowDisplayMode(displayMode), showEmptyLabel
    );
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "audio_visualizer") {
    const float width = static_cast<float>(wc != nullptr ? wc->getDouble("width", 56.0) : 56.0);
    const int bands = static_cast<int>(wc != nullptr ? wc->getInt("bands", 16) : 16);
    const bool mirrored = wc != nullptr ? wc->getBool("mirrored", true) : true;
    const bool centered = wc != nullptr ? wc->getBool("centered", true) : true;
    const bool showWhenIdle = wc != nullptr ? wc->getBool("show_when_idle", false) : false;
    const ColorSpec color1 = wc != nullptr
        ? wc->getColorSpec("color_1", colorSpecFromRole(ColorRole::Primary), "widget." + name + ".color_1")
        : colorSpecFromRole(ColorRole::Primary);
    const ColorSpec color2 = wc != nullptr
        ? wc->getColorSpec("color_2", colorSpecFromRole(ColorRole::Primary), "widget." + name + ".color_2")
        : colorSpecFromRole(ColorRole::Primary);
    auto widget = std::make_unique<AudioVisualizerWidget>(
        m_audioSpectrum,
        AudioVisualizerWidget::Options{
            .width = width,
            .bands = bands,
            .mirrored = mirrored,
            .centered = centered,
            .showWhenIdle = showWhenIdle,
            .color1 = color1,
            .color2 = color2,
        }
    );
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "battery") {
    const std::string deviceSelector = wc != nullptr ? wc->getString("device", "auto") : std::string("auto");
    const int warningThreshold = batteryWarningThresholdForSelector(m_config.battery, m_upower, deviceSelector);
    const ColorSpec warningColor = wc != nullptr
        ? wc->getColorSpec("warning_color", colorSpecFromRole(ColorRole::Error), "widget." + name + ".warning_color")
        : colorSpecFromRole(ColorRole::Error);
    const std::string displayModeStr = wc != nullptr ? wc->getString("display_mode", "glyph") : std::string("glyph");
    const bool showLabel = wc != nullptr ? wc->getBool("show_label", true) : true;
    const bool hideWhenPlugged = wc != nullptr ? wc->getBool("hide_when_plugged", false) : false;
    const bool hideWhenFull = wc != nullptr ? wc->getBool("hide_when_full", false) : false;
    BatteryDisplayMode displayMode = BatteryDisplayMode::Glyph;
    if (displayModeStr == "graphic") {
      displayMode = BatteryDisplayMode::Graphic;
    } else if (displayModeStr != "glyph") {
      kLog.warn("invalid widget.{}.display_mode '{}'; expected glyph or graphic", name, displayModeStr);
    }
    auto widget = std::make_unique<BatteryWidget>(
        m_upower, deviceSelector, warningThreshold, warningColor, displayMode, showLabel, hideWhenPlugged, hideWhenFull
    );
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "bluetooth") {
    const bool showLabel = wc != nullptr ? wc->getBool("show_label", false) : false;
    const bool hideWhenNoConnectedDevice = wc != nullptr ? wc->getBool("hide_when_no_connected_device", false) : false;
    auto widget = std::make_unique<BluetoothWidget>(m_bluetooth, output, showLabel, hideWhenNoConnectedDevice);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "brightness") {
    const bool showLabel = wc != nullptr ? wc->getBool("show_label", true) : true;
    const int scrollStep =
        static_cast<int>(std::clamp<std::int64_t>(wc != nullptr ? wc->getInt("scroll_step", 5) : 5, 1, 25));
    auto widget = std::make_unique<BrightnessWidget>(m_brightness, output, showLabel, scrollStep);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "clock") {
    std::string format = wc != nullptr ? wc->getString("format", "{:%H:%M}") : std::string("{:%H:%M}");
    std::string verticalFormat = wc != nullptr ? wc->getString("vertical_format", "") : std::string{};
    std::string tooltipFormat = wc != nullptr ? wc->getString("tooltip_format", "") : std::string{};
    auto widget =
        std::make_unique<ClockWidget>(output, std::move(format), std::move(verticalFormat), std::move(tooltipFormat));
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "clipboard") {
    if (!m_config.shell.clipboardEnabled) {
      return nullptr;
    }
    auto barGlyph = wc != nullptr ? wc->getString("glyph", "clipboard") : std::string{"clipboard"};
    if (barGlyph.empty()) {
      barGlyph = "clipboard";
    }
    auto widget = std::make_unique<ClipboardWidget>(output, std::move(barGlyph));
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "control-center") {
    auto barGlyph = wc != nullptr ? wc->getString("glyph", "noctalia") : std::string{"noctalia"};
    if (barGlyph.empty()) {
      barGlyph = "search";
    }

    std::string logoPath = wc != nullptr ? wc->getString("custom_image", "") : std::string{};
    const bool customImageColorize = wc != nullptr ? wc->getBool("custom_image_colorize", false) : false;

    auto widget =
        std::make_unique<ControlCenterWidget>(output, std::move(barGlyph), std::move(logoPath), customImageColorize);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "custom_button") {
    auto trimSetting = [wc](const char* key, const char* fallback = "") {
      return wc != nullptr ? StringUtils::trim(wc->getString(key, fallback)) : std::string(fallback);
    };
    auto widget = std::make_unique<CustomButtonWidget>(CustomButtonWidget::Options{
        .glyph = trimSetting("glyph", "heart"),
        .label = trimSetting("label"),
        .tooltip = trimSetting("tooltip"),
        .command = trimSetting("command"),
        .rightCommand = trimSetting("right_command"),
        .middleCommand = trimSetting("middle_command"),
        .scrollUpCommand = trimSetting("scroll_up_command"),
        .scrollDownCommand = trimSetting("scroll_down_command"),
    });
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "caffeine") {
    auto widget = std::make_unique<IdleInhibitorWidget>(m_idleInhibitor);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "keyboard_layout") {
    const std::string cycleCommand = wc != nullptr ? wc->getString("cycle_command", "") : std::string{};
    const std::string display = wc != nullptr ? wc->getString("display", "short") : std::string("short");
    const bool showIcon = wc != nullptr ? wc->getBool("show_icon", true) : true;
    const bool showLabel = wc != nullptr ? wc->getBool("show_label", true) : true;
    const bool hideWhenSingleLayout = wc != nullptr ? wc->getBool("hide_when_single_layout", false) : false;
    auto customLabels =
        wc != nullptr ? wc->getStringMap("custom_labels") : std::unordered_map<std::string, std::string>{};
    std::string glyph = wc != nullptr ? wc->getString("glyph", "keyboard") : std::string{"keyboard"};
    if (glyph.empty()) {
      glyph = "keyboard";
    }
    auto widget = std::make_unique<KeyboardLayoutWidget>(
        m_platform, cycleCommand, KeyboardLayoutWidget::parseDisplayMode(display), showIcon, showLabel,
        hideWhenSingleLayout, std::move(customLabels), std::move(glyph)
    );
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "launcher") {
    auto barGlyph = wc != nullptr ? wc->getString("glyph", "search") : std::string{"search"};
    if (barGlyph.empty()) {
      barGlyph = "search";
    }

    std::string logoPath = wc != nullptr ? wc->getString("custom_image", "") : std::string{};
    const bool customImageColorize = wc != nullptr ? wc->getBool("custom_image_colorize", false) : false;

    auto widget =
        std::make_unique<LauncherWidget>(output, std::move(barGlyph), std::move(logoPath), customImageColorize);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "lock_keys") {
    if (m_lockKeys == nullptr) {
      return nullptr;
    }
    const bool showCaps = wc != nullptr ? wc->getBool("show_caps_lock", true) : true;
    const bool showNum = wc != nullptr ? wc->getBool("show_num_lock", true) : true;
    const bool showScroll = wc != nullptr ? wc->getBool("show_scroll_lock", false) : false;
    const bool hideWhenOff = wc != nullptr ? wc->getBool("hide_when_off", false) : false;
    const std::string display = wc != nullptr ? wc->getString("display", "short") : std::string("short");

    auto widget = std::make_unique<LockKeysWidget>(
        m_lockKeys, showCaps, showNum, showScroll, hideWhenOff, LockKeysWidget::parseDisplayMode(display)
    );
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "media") {
    const float maxWidth = static_cast<float>(wc != nullptr ? wc->getDouble("max_length", 220.0) : 220.0);
    const float minWidth = static_cast<float>(wc != nullptr ? wc->getDouble("min_length", 80.0) : 80.0);
    const float artSize = static_cast<float>(wc != nullptr ? wc->getDouble("art_size", 16.0) : 16.0);
    const std::string titleScroll = wc != nullptr ? wc->getString("title_scroll", "none") : std::string("none");
    const bool hideWhenNoMedia = wc != nullptr ? wc->getBool("hide_when_no_media", false) : false;
    const bool albumArtOnly = wc != nullptr ? wc->getBool("album_art_only", false) : false;
    const bool hideAlbumArt = wc != nullptr ? wc->getBool("hide_album_art", false) : false;
    auto widget = std::make_unique<MediaWidget>(
        m_mpris, m_httpClient, output, maxWidth, minWidth, artSize, parseMediaTitleScrollMode(titleScroll),
        hideWhenNoMedia, albumArtOnly, hideAlbumArt
    );
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "network") {
    const bool showLabel = wc != nullptr ? wc->getBool("show_label", true) : true;
    auto widget = std::make_unique<NetworkWidget>(m_network, m_sysmon, output, showLabel);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "nightlight") {
    auto widget = std::make_unique<NightLightWidget>(m_nightLight);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "notifications") {
    const bool hideWhenNoUnread = wc != nullptr ? wc->getBool("hide_when_no_unread", false) : false;
    auto widget = std::make_unique<NotificationWidget>(m_notifications, output, hideWhenNoUnread);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "power_profile") {
    auto widget = std::make_unique<PowerProfileWidget>(m_powerProfiles);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "privacy") {
    PrivacyWidgetConfig config;

    if (wc != nullptr) {
      config.hideInactive = wc->getBool("hide_inactive", config.hideInactive);
      config.iconSpacing =
          static_cast<int>(std::clamp<std::int64_t>(wc->getInt("icon_spacing", config.iconSpacing), 0, 48));
      config.activeColor = wc->getColorSpec("active_color", config.activeColor, "widget." + name + ".active_color");
      config.inactiveColor =
          wc->getColorSpec("inactive_color", config.inactiveColor, "widget." + name + ".inactive_color");
    }

    auto widget = std::make_unique<PrivacyWidget>(m_audio, &m_configService, config);
    widget->setContentScale(contentScale);
    return widget;
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
        barName, outputName
    );
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "screenshot") {
    if (m_screenshots == nullptr || m_renderContext == nullptr || !m_screenshots->available()) {
      return nullptr;
    }
    auto barGlyph = wc != nullptr ? wc->getString("glyph", "screenshot") : std::string{"screenshot"};
    if (barGlyph.empty()) {
      barGlyph = "screenshot";
    }
    auto widget = std::make_unique<ScreenshotWidget>(
        output, std::move(barGlyph), *m_screenshots, m_configService, m_platform, *m_renderContext,
        m_configService.config().shell.shadow, barPosition
    );
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "session") {
    auto barGlyph = wc != nullptr ? wc->getString("glyph", "shutdown") : std::string{"shutdown"};
    if (barGlyph.empty()) {
      barGlyph = "shutdown";
    }
    auto widget = std::make_unique<SessionWidget>(output, std::move(barGlyph));
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "settings") {
    auto barGlyph = wc != nullptr ? wc->getString("glyph", "settings") : std::string{"settings"};
    if (barGlyph.empty()) {
      barGlyph = "search";
    }
    auto widget = std::make_unique<SettingsWidget>(output, std::move(barGlyph));
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "spacer") {
    constexpr double kDefaultSpacerLength = 20.0;
    const auto length =
        static_cast<float>(wc != nullptr ? wc->getDouble("length", kDefaultSpacerLength) : kDefaultSpacerLength);
    const bool verticalBar = barPosition == "left" || barPosition == "right";
    auto widget = std::make_unique<SpacerWidget>(length, verticalBar);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "sysmon") {
    std::string statStr = wc != nullptr ? wc->getString("stat", "cpu_usage") : std::string("cpu_usage");
    std::string path = wc != nullptr ? wc->getString("path", "/") : std::string("/");
    SysmonStat stat = SysmonStat::CpuUsage;
    if (statStr == "cpu_temp") {
      stat = SysmonStat::CpuTemp;
    } else if (statStr == "gpu_temp") {
      stat = SysmonStat::GpuTemp;
    } else if (statStr == "gpu_usage") {
      stat = SysmonStat::GpuUsage;
    } else if (statStr == "gpu_vram") {
      stat = SysmonStat::GpuVram;
    } else if (statStr == "ram_used") {
      stat = SysmonStat::RamUsed;
    } else if (statStr == "ram_pct") {
      stat = SysmonStat::RamPct;
    } else if (statStr == "swap_pct") {
      stat = SysmonStat::SwapPct;
    } else if (statStr == "disk_pct") {
      stat = SysmonStat::DiskPct;
    } else if (statStr == "net_rx") {
      stat = SysmonStat::NetRx;
    } else if (statStr == "net_tx") {
      stat = SysmonStat::NetTx;
    }
    const std::string display = wc != nullptr ? wc->getString("display", "gauge") : std::string("gauge");
    const std::string networkInterface = wc != nullptr ? wc->getString("interface", "") : std::string();
    SysmonDisplayMode displayMode = SysmonDisplayMode::Gauge;
    if (display == "text")
      displayMode = SysmonDisplayMode::Text;
    else if (display == "graph")
      displayMode = SysmonDisplayMode::Graph;
    SysmonWidgetOptions options{
        .stat = stat,
        .diskPath = std::move(path),
        .displayMode = displayMode,
        .highlightColor = wc != nullptr
            ? wc->getColorSpec(
                  "highlight_color", colorSpecFromRole(ColorRole::Error), "widget." + name + ".highlight_color"
              )
            : colorSpecFromRole(ColorRole::Error),
        .networkInterface = networkInterface,
        .showLabel = wc != nullptr ? wc->getBool("show_label", true) : true,
        .labelMinWidth = static_cast<float>(wc != nullptr ? wc->getDouble("label_min_width", 0.0) : 0.0),
        .glyph = wc != nullptr ? wc->getString("glyph", "") : std::string{},
    };
    auto widget = std::make_unique<SysmonWidget>(m_sysmon, m_configService, std::move(options));
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "test") {
    auto widget = std::make_unique<TestWidget>(output);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "taskbar") {
    TaskbarWidgetOptions options{
        .groupByWorkspace = wc != nullptr ? wc->getBool("group_by_workspace", false) : false,
        .showAllOutputs = wc != nullptr ? wc->getBool("show_all_outputs", false) : false,
        .onlyActiveWorkspace = wc != nullptr ? wc->getBool("only_active_workspace", false) : false,
        .showWorkspaceLabel = wc != nullptr ? wc->getBool("show_workspace_label", true) : true,
        .workspaceLabelPlacement = WorkspaceLabelPlacement::Corner,
        .hideEmptyWorkspaces = wc != nullptr ? wc->getBool("hide_empty_workspaces", false) : false,
        .workspaceGroupCapsule = wc != nullptr ? wc->getBool("workspace_group_capsule", true) : true,
        .focusedOutputOnly = wc != nullptr ? wc->getBool("focused_output_only", false) : false,
        .groupSingleIconPerApp = wc != nullptr ? wc->getBool("group_single_icon_per_app", false) : false,
        .showActiveIndicator = wc != nullptr ? wc->getBool("show_active_indicator", true) : true,
        .activeOpacity = wc != nullptr ? static_cast<float>(wc->getDouble("active_opacity", 1.0)) : 1.0f,
        .inactiveOpacity = wc != nullptr ? static_cast<float>(wc->getDouble("inactive_opacity", 1.0)) : 1.0f,
        .focusedColor = wc != nullptr
            ? wc->getColorSpec(
                  "focused_color", colorSpecFromRole(ColorRole::Primary), "widget." + name + ".focused_color"
              )
            : colorSpecFromRole(ColorRole::Primary),
        .occupiedColor = wc != nullptr
            ? wc->getColorSpec(
                  "occupied_color", colorSpecFromRole(ColorRole::Secondary), "widget." + name + ".occupied_color"
              )
            : colorSpecFromRole(ColorRole::Secondary),
        .emptyColor = wc != nullptr
            ? wc->getColorSpec(
                  "empty_color", colorSpecFromRole(ColorRole::Secondary), "widget." + name + ".empty_color"
              )
            : colorSpecFromRole(ColorRole::Secondary),
        .showWindowTitle = wc != nullptr ? wc->getBool("show_window_title", false) : false,
        .windowTitleMaxWidth =
            static_cast<float>(wc != nullptr ? wc->getDouble("window_title_max_width", 100.0) : 100.0),
        .taskbarMaxWidth = static_cast<float>(wc != nullptr ? wc->getDouble("taskbar_max_width", 8192.0) : 8192.0),
        .barPosition = barPosition,
        .shadowConfig = m_config.shell.shadow,
    };
    if (wc != nullptr) {
      const std::string placement = wc->getString("workspace_label_placement", "corner");
      if (placement == "centered") {
        options.workspaceLabelPlacement = WorkspaceLabelPlacement::Centered;
      } else if (placement == "inside") {
        options.workspaceLabelPlacement = WorkspaceLabelPlacement::Inside;
      }
    }
    auto widget = std::make_unique<TaskbarWidget>(m_platform, m_configService, output, std::move(options));
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "theme_mode") {
    auto widget = std::make_unique<ThemeModeWidget>(m_themeService);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "tray") {
    TrayWidgetOptions options{
        .hiddenItems = wc != nullptr ? wc->getStringList("hidden") : std::vector<std::string>{},
        .pinnedItems = wc != nullptr ? wc->getStringList("pinned") : std::vector<std::string>{},
        .drawerMode = wc != nullptr ? wc->getBool("drawer", false) : false,
        .itemActivated = {},
        .barPosition = barPosition,
        .panelGridMode = false,
        .panelGridColumns = static_cast<std::size_t>(
            std::clamp<std::int64_t>(wc != nullptr ? wc->getInt("drawer_columns", 3) : 3, 1, 5)
        ),
        .inlineEntryGap = widgetSpacing,
        .matchAdjacentSpacing = wc != nullptr ? wc->getBool("match_adjacent_spacing", false) : false,
    };
    auto widget = std::make_unique<TrayWidget>(m_configService, m_tray, std::move(options));
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "volume") {
    const bool showLabel = wc != nullptr ? wc->getBool("show_label", true) : true;
    const int scrollStep =
        static_cast<int>(std::clamp<std::int64_t>(wc != nullptr ? wc->getInt("scroll_step", 5) : 5, 1, 25));
    const std::string target = wc != nullptr ? wc->getString("device", "output") : std::string("output");
    const auto volumeTarget = target == "input" ? VolumeWidgetTarget::Input : VolumeWidgetTarget::Output;
    const ColorSpec muteColor = wc != nullptr
        ? wc->getColorSpec("mute_color", colorSpecFromRole(ColorRole::Error), "widget." + name + ".mute_color")
        : colorSpecFromRole(ColorRole::Error);
    auto widget = std::make_unique<VolumeWidget>(
        m_audio, m_easyEffects, &m_config, output, showLabel, volumeTarget, scrollStep, muteColor
    );
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "wallpaper") {
    auto barGlyph = wc != nullptr ? wc->getString("glyph", "wallpaper-selector") : std::string{"wallpaper-selector"};
    if (barGlyph.empty()) {
      barGlyph = "wallpaper-selector";
    }
    auto widget = std::make_unique<WallpaperWidget>(output, std::move(barGlyph));
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "weather") {
    const float maxWidth = static_cast<float>(wc != nullptr ? wc->getDouble("max_length", 160.0) : 160.0);
    const bool showCondition = wc != nullptr ? wc->getBool("show_condition", true) : true;
    auto widget = std::make_unique<WeatherWidget>(m_weather, output, maxWidth, showCondition);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "workspaces") {
    const std::string display = wc != nullptr ? wc->getString("display", "id") : std::string("id");
    const ColorSpec focusedColor = wc != nullptr
        ? wc->getColorSpec("focused_color", colorSpecFromRole(ColorRole::Primary), "widget." + name + ".focused_color")
        : colorSpecFromRole(ColorRole::Primary);
    const ColorSpec occupiedColor = wc != nullptr
        ? wc->getColorSpec(
              "occupied_color", colorSpecFromRole(ColorRole::Secondary), "widget." + name + ".occupied_color"
          )
        : colorSpecFromRole(ColorRole::Secondary);
    const ColorSpec emptyColor = wc != nullptr
        ? wc->getColorSpec("empty_color", colorSpecFromRole(ColorRole::Secondary), "widget." + name + ".empty_color")
        : colorSpecFromRole(ColorRole::Secondary);
    WorkspacesWidget::DisplayMode displayMode = WorkspacesWidget::DisplayMode::Id;
    if (display == "id") {
      displayMode = WorkspacesWidget::DisplayMode::Id;
    } else if (display == "name") {
      displayMode = WorkspacesWidget::DisplayMode::Name;
    } else if (display == "none") {
      displayMode = WorkspacesWidget::DisplayMode::None;
    }
    std::size_t maxLabelChars = 1; // Default: truncate names to 1 char (v4 behavior)
    if (wc != nullptr && wc->hasSetting("max_label_chars")) {
      maxLabelChars = static_cast<std::size_t>(wc->getInt("max_label_chars", 1));
    }
    WorkspacesWidget::Options options{
        .displayMode = displayMode,
        .focusedColor = focusedColor,
        .occupiedColor = occupiedColor,
        .emptyColor = emptyColor,
        .maxLabelChars = maxLabelChars,
        .labelsOnlyWhenOccupied = wc != nullptr ? wc->getBool("labels_only_when_occupied", false) : false,
        .hideWhenEmpty = wc != nullptr ? wc->getBool("hide_when_empty", false) : false,
        .pillScale = static_cast<float>(wc != nullptr ? wc->getDouble("pill_scale", 1.0) : 1.0),
        .activePillSize = static_cast<float>(wc != nullptr ? wc->getDouble("active_pill_size", 2.2) : 2.2),
        .inactivePillSize = static_cast<float>(wc != nullptr ? wc->getDouble("inactive_pill_size", 1.0) : 1.0),
        .minimal = wc != nullptr ? wc->getBool("minimal", false) : false,
        .focusedOutputOnly = wc != nullptr ? wc->getBool("focused_output_only", false) : false,
    };
    auto widget = std::make_unique<WorkspacesWidget>(m_platform, output, options);
    widget->setContentScale(contentScale);
    return widget;
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
