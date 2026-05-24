#include "shell/bar/widget_factory.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/log.h"
#include "dbus/mpris/mpris_service.h"
#include "dbus/power/power_profiles_service.h"
#include "dbus/tray/tray_service.h"
#include "idle/idle_inhibitor.h"
#include "net/http_client.h"
#include "notification/notification_manager.h"
#include "pipewire/pipewire_spectrum.h"
#include "shell/bar/widgets/active_window_widget.h"
#include "shell/bar/widgets/audio_visualizer_widget.h"
#include "shell/bar/widgets/battery_widget.h"
#include "shell/bar/widgets/bluetooth_widget.h"
#include "shell/bar/widgets/brightness_widget.h"
#include "shell/bar/widgets/clipboard_widget.h"
#include "shell/bar/widgets/clock_widget.h"
#include "shell/bar/widgets/control_center_widget.h"
#include "shell/bar/widgets/custom_button_widget.h"
#ifndef NDEBUG
#include "shell/bar/widgets/debug_indicator_widget.h"
#endif
#include "shell/bar/widgets/idle_inhibitor_widget.h"
#include "shell/bar/widgets/keyboard_layout_widget.h"
#include "shell/bar/widgets/launcher_widget.h"
#include "shell/bar/widgets/lock_keys_widget.h"
#include "shell/bar/widgets/media_widget.h"
#include "shell/bar/widgets/network_widget.h"
#include "shell/bar/widgets/nightlight_widget.h"
#include "shell/bar/widgets/notification_widget.h"
#include "shell/bar/widgets/power_profile_widget.h"
#include "shell/bar/widgets/scripted_widget.h"
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
#include "system/lock_keys_service.h"
#include "system/system_monitor_service.h"
#include "system/weather_service.h"
#include "theme/theme_service.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

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

WidgetFactory::WidgetFactory(CompositorPlatform& platform, const Config& config, NotificationManager* notifications,
                             TrayService* tray, PipeWireService* audio, UPowerService* upower,
                             SystemMonitorService* sysmon, PowerProfilesService* powerProfiles,
                             INetworkService* network, IdleInhibitor* idleInhibitor, MprisService* mpris,
                             PipeWireSpectrum* audioSpectrum, HttpClient* httpClient, WeatherService* weather,
                             GammaService* nightLight, noctalia::theme::ThemeService* themeService,
                             BluetoothService* bluetooth, BrightnessService* brightness, LockKeysService* lockKeys,
                             ClipboardService* clipboard, FileWatcher* fileWatcher)
    : m_platform(platform), m_config(config), m_notifications(notifications), m_tray(tray), m_audio(audio),
      m_upower(upower), m_sysmon(sysmon), m_powerProfiles(powerProfiles), m_network(network),
      m_idleInhibitor(idleInhibitor), m_mpris(mpris), m_audioSpectrum(audioSpectrum), m_httpClient(httpClient),
      m_weather(weather), m_nightLight(nightLight), m_themeService(themeService), m_bluetooth(bluetooth),
      m_brightness(brightness), m_lockKeys(lockKeys), m_clipboard(clipboard), m_fileWatcher(fileWatcher) {}

WidgetFactory::~WidgetFactory() = default;

std::unique_ptr<Widget> WidgetFactory::create(const std::string& name, wl_output* output, float contentScale,
                                              const std::string& barPosition, const std::string& barName,
                                              float widgetSpacing) const {
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
    auto widget = std::make_unique<ActiveWindowWidget>(m_platform, maxWidth, minWidth, iconSize,
                                                       parseActiveWindowTitleScrollMode(titleScroll),
                                                       parseActiveWindowDisplayMode(displayMode));
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "audio_visualizer") {
    const float width = static_cast<float>(wc != nullptr ? wc->getDouble("width", 56.0) : 56.0);
    const int bands = static_cast<int>(wc != nullptr ? wc->getInt("bands", 16) : 16);
    const bool mirrored = wc != nullptr ? wc->getBool("mirrored", true) : true;
    const bool centered = wc != nullptr ? wc->getBool("centered", true) : true;
    const bool showWhenIdle = wc != nullptr ? wc->getBool("show_when_idle", false) : false;
    const ColorSpec lowColor = wc != nullptr ? wc->getColorSpec("low_color", colorSpecFromRole(ColorRole::Primary),
                                                                "widget." + name + ".low_color")
                                             : colorSpecFromRole(ColorRole::Primary);
    const ColorSpec highColor = wc != nullptr ? wc->getColorSpec("high_color", colorSpecFromRole(ColorRole::Primary),
                                                                 "widget." + name + ".high_color")
                                              : colorSpecFromRole(ColorRole::Primary);
    auto widget = std::make_unique<AudioVisualizerWidget>(m_audioSpectrum, width, bands, mirrored, lowColor, highColor,
                                                          centered, showWhenIdle);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "battery") {
    const std::string deviceSelector = wc != nullptr ? wc->getString("device", "auto") : std::string("auto");
    const int warningThreshold = static_cast<int>(wc != nullptr ? wc->getInt("warning_threshold", 20) : 20);
    const ColorSpec warningColor = wc != nullptr
                                       ? wc->getColorSpec("warning_color", colorSpecFromRole(ColorRole::Error),
                                                          "widget." + name + ".warning_color")
                                       : colorSpecFromRole(ColorRole::Error);
    const std::string displayModeStr = wc != nullptr ? wc->getString("display_mode", "icon") : std::string("icon");
    const bool showLabel = wc != nullptr ? wc->getBool("show_label", true) : true;
    const bool hideWhenPlugged = wc != nullptr ? wc->getBool("hide_when_plugged", false) : false;
    const bool hideWhenFull = wc != nullptr ? wc->getBool("hide_when_full", false) : false;
    const BatteryDisplayMode displayMode =
        displayModeStr == "graphic" ? BatteryDisplayMode::Graphic : BatteryDisplayMode::Icon;
    auto widget = std::make_unique<BatteryWidget>(m_upower, deviceSelector, warningThreshold, warningColor, displayMode,
                                                  showLabel, hideWhenPlugged, hideWhenFull);
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
    auto widget = std::make_unique<BrightnessWidget>(m_brightness, output, showLabel);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "clock") {
    std::string format = wc != nullptr ? wc->getString("format", "{:%H:%M}") : std::string("{:%H:%M}");
    std::string verticalFormat = wc != nullptr ? wc->getString("vertical_format", "") : std::string{};
    auto widget = std::make_unique<ClockWidget>(output, std::move(format), std::move(verticalFormat));
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

    auto widget = std::make_unique<ControlCenterWidget>(output, std::move(barGlyph), std::move(logoPath));
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "custom_button") {
    auto trimSetting = [wc](const char* key, const char* fallback = "") {
      return wc != nullptr ? StringUtils::trim(wc->getString(key, fallback)) : std::string(fallback);
    };
    bool autoReloadImage = wc != nullptr ? wc->getBool("image_auto_reload", false) : false;
    auto widget = std::make_unique<CustomButtonWidget>(
        trimSetting("glyph", "heart"), trimSetting("label"), trimSetting("tooltip"), trimSetting("image_path"),
        autoReloadImage, trimSetting("command"), trimSetting("right_command"), trimSetting("middle_command"),
        trimSetting("scroll_up_command"), trimSetting("scroll_down_command"));
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
    const bool hideLabel = wc != nullptr ? wc->getBool("hide_label", false) : false;
    auto widget = std::make_unique<KeyboardLayoutWidget>(m_platform, cycleCommand,
                                                         KeyboardLayoutWidget::parseDisplayMode(display), hideLabel);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "launcher") {
    auto barGlyph = wc != nullptr ? wc->getString("glyph", "search") : std::string{"search"};
    if (barGlyph.empty()) {
      barGlyph = "search";
    }

    std::string logoPath = wc != nullptr ? wc->getString("custom_image", "") : std::string{};

    auto widget = std::make_unique<LauncherWidget>(output, std::move(barGlyph), std::move(logoPath));
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

    auto widget = std::make_unique<LockKeysWidget>(m_lockKeys, showCaps, showNum, showScroll, hideWhenOff,
                                                   LockKeysWidget::parseDisplayMode(display));
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "media") {
    const float maxWidth = static_cast<float>(wc != nullptr ? wc->getDouble("max_length", 220.0) : 220.0);
    const float minWidth = static_cast<float>(wc != nullptr ? wc->getDouble("min_length", 80.0) : 80.0);
    const float artSize = static_cast<float>(wc != nullptr ? wc->getDouble("art_size", 16.0) : 16.0);
    const std::string titleScroll = wc != nullptr ? wc->getString("title_scroll", "none") : std::string("none");
    const bool hideWhenNoMedia = wc != nullptr ? wc->getBool("hide_when_no_media", false) : false;
    auto widget = std::make_unique<MediaWidget>(m_mpris, m_httpClient, output, maxWidth, minWidth, artSize,
                                                parseMediaTitleScrollMode(titleScroll), hideWhenNoMedia);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "network") {
    const bool showLabel = wc != nullptr ? wc->getBool("show_label", true) : true;
    auto widget = std::make_unique<NetworkWidget>(m_network, output, showLabel);
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

  if (type == "scripted") {
    std::string script = wc != nullptr ? wc->getString("script", "") : std::string();
    const auto* outputInfo = m_platform.findOutputByWl(output);
    const std::string outputName = outputInfo != nullptr ? outputInfo->connectorName : std::string{};
    auto widget = std::make_unique<ScriptedWidget>(name, std::move(script), barName, outputName, wc, m_fileWatcher,
                                                   &m_platform, m_clipboard, m_audioSpectrum, m_mpris);
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
    SysmonDisplayMode displayMode = SysmonDisplayMode::Gauge;
    if (display == "text")
      displayMode = SysmonDisplayMode::Text;
    else if (display == "graph")
      displayMode = SysmonDisplayMode::Graph;
    const bool showLabel = wc != nullptr ? wc->getBool("show_label", true) : true;
    const auto labelMinWidth = static_cast<float>(wc != nullptr ? wc->getDouble("label_min_width", 0.0) : 0.0);
    auto widget =
        std::make_unique<SysmonWidget>(m_sysmon, output, stat, std::move(path), displayMode, showLabel, labelMinWidth);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "test") {
    auto widget = std::make_unique<TestWidget>(output);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "taskbar") {
    const bool groupByWorkspace = wc != nullptr ? wc->getBool("group_by_workspace", false) : false;
    const bool showAllOutputs = wc != nullptr ? wc->getBool("show_all_outputs", false) : false;
    const bool onlyActiveWorkspace = wc != nullptr ? wc->getBool("only_active_workspace", false) : false;
    const bool showWorkspaceLabel = wc != nullptr ? wc->getBool("show_workspace_label", true) : true;
    WorkspaceLabelPlacement workspaceLabelPlacement = WorkspaceLabelPlacement::Corner;
    if (wc != nullptr) {
      const std::string placement = wc->getString("workspace_label_placement", "corner");
      if (placement == "centered") {
        workspaceLabelPlacement = WorkspaceLabelPlacement::Centered;
      } else if (placement == "inside") {
        workspaceLabelPlacement = WorkspaceLabelPlacement::Inside;
      }
    }
    const bool hideEmptyWorkspaces = wc != nullptr ? wc->getBool("hide_empty_workspaces", false) : false;
    const bool workspaceGroupCapsule = wc != nullptr ? wc->getBool("workspace_group_capsule", true) : true;
    const ColorSpec focusedColor = wc != nullptr
                                       ? wc->getColorSpec("focused_color", colorSpecFromRole(ColorRole::Primary),
                                                          "widget." + name + ".focused_color")
                                       : colorSpecFromRole(ColorRole::Primary);
    const ColorSpec occupiedColor = wc != nullptr
                                        ? wc->getColorSpec("occupied_color", colorSpecFromRole(ColorRole::Secondary),
                                                           "widget." + name + ".occupied_color")
                                        : colorSpecFromRole(ColorRole::Secondary);
    const ColorSpec emptyColor = wc != nullptr
                                     ? wc->getColorSpec("empty_color", colorSpecFromRole(ColorRole::Secondary),
                                                        "widget." + name + ".empty_color")
                                     : colorSpecFromRole(ColorRole::Secondary);
    auto widget = std::make_unique<TaskbarWidget>(m_platform, output, groupByWorkspace, showAllOutputs,
                                                  onlyActiveWorkspace, showWorkspaceLabel, workspaceLabelPlacement,
                                                  hideEmptyWorkspaces, workspaceGroupCapsule, focusedColor,
                                                  occupiedColor, emptyColor, barPosition, m_config.shell.shadow);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "theme_mode") {
    auto widget = std::make_unique<ThemeModeWidget>(m_themeService);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "tray") {
    const auto hiddenItems = wc != nullptr ? wc->getStringList("hidden") : std::vector<std::string>{};
    const auto pinnedItems = wc != nullptr ? wc->getStringList("pinned") : std::vector<std::string>{};
    const bool drawer = wc != nullptr ? wc->getBool("drawer", false) : false;
    const std::size_t drawerColumns =
        static_cast<std::size_t>(std::clamp<std::int64_t>(wc != nullptr ? wc->getInt("drawer_columns", 3) : 3, 1, 5));
    const bool matchAdjacentSpacing = wc != nullptr ? wc->getBool("match_adjacent_spacing", false) : false;
    auto widget = std::make_unique<TrayWidget>(m_tray, hiddenItems, pinnedItems, drawer, std::function<void()>{},
                                               barPosition, false, drawerColumns, widgetSpacing, matchAdjacentSpacing);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "volume") {
    const bool showLabel = wc != nullptr ? wc->getBool("show_label", true) : true;
    const std::string target = wc != nullptr ? wc->getString("device", "output") : std::string("output");
    const auto volumeTarget = target == "input" ? VolumeWidgetTarget::Input : VolumeWidgetTarget::Output;
    auto widget = std::make_unique<VolumeWidget>(m_audio, &m_config, output, showLabel, volumeTarget);
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
                                       ? wc->getColorSpec("focused_color", colorSpecFromRole(ColorRole::Primary),
                                                          "widget." + name + ".focused_color")
                                       : colorSpecFromRole(ColorRole::Primary);
    const ColorSpec occupiedColor = wc != nullptr
                                        ? wc->getColorSpec("occupied_color", colorSpecFromRole(ColorRole::Secondary),
                                                           "widget." + name + ".occupied_color")
                                        : colorSpecFromRole(ColorRole::Secondary);
    const ColorSpec emptyColor = wc != nullptr
                                     ? wc->getColorSpec("empty_color", colorSpecFromRole(ColorRole::Secondary),
                                                        "widget." + name + ".empty_color")
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
    const bool hideWhenEmpty = wc != nullptr ? wc->getBool("hide_when_empty", false) : false;
    const double pillScale = wc != nullptr ? wc->getDouble("pill_scale", 1.0) : 1.0;
    const bool minimal = wc != nullptr ? wc->getBool("minimal", false) : false;
    auto widget = std::make_unique<WorkspacesWidget>(m_platform, output, displayMode, focusedColor, occupiedColor,
                                                     emptyColor, maxLabelChars, hideWhenEmpty, pillScale, minimal);
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
