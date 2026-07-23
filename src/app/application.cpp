#include "application.h"

#include "app/main_loop.h"
#include "compositors/compositor_detect.h"
#include "config/config_types.h"
#include "core/build_info.h"
#include "core/deferred_call.h"
#include "core/files/resource_paths.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/process/process.h"
#include "cursor-shape-v1-client-protocol.h"
#include "dbus/accounts/accounts_service.h"
#include "dbus/bluetooth/bluetooth_agent.h"
#include "dbus/bluetooth/bluetooth_service.h"
#include "dbus/idle/screensaver_poll_source.h"
#include "dbus/idle/screensaver_service.h"
#include "dbus/logind/logind_service.h"
#include "dbus/mpris/mpris_service.h"
#include "dbus/network/inetwork_service.h"
#include "dbus/network/iwd_secret_agent.h"
#include "dbus/network/network_manager_service.h"
#include "dbus/network/network_secret_agent.h"
#include "dbus/network/wpa_supplicant_service.h"
#include "dbus/notification/kde_notification_client.h"
#include "dbus/notification/notification_dbus_host.h"
#include "dbus/notification/notification_service.h"
#include "dbus/polkit/polkit_agent.h"
#include "dbus/polkit/polkit_poll_source.h"
#include "dbus/polkit/polkit_session_support.h"
#include "dbus/power/power_profiles_service.h"
#include "dbus/session_bus.h"
#include "dbus/session_bus_poll_source.h"
#include "dbus/system_bus.h"
#include "dbus/system_bus_poll_source.h"
#include "dbus/tray/tray_service.h"
#include "dbus/upower/upower_service.h"
#include "debug/debug_service.h"
#include "i18n/i18n.h"
#include "i18n/i18n_service.h"
#include "ipc/ipc_arg_parse.h"
#include "launcher/app_provider.h"
#include "launcher/dmenu_provider.h"
#include "launcher/emoji_provider.h"
#include "launcher/math_provider.h"
#include "launcher/plugin_launcher_provider.h"
#include "launcher/session_provider.h"
#include "launcher/wallpaper_provider.h"
#include "launcher/window_provider.h"
#include "notification/notifications.h"
#include "pipewire/pipewire_poll_source.h"
#include "pipewire/pipewire_service.h"
#include "pipewire/pipewire_spectrum.h"
#include "pipewire/pipewire_spectrum_poll_source.h"
#include "pipewire/sound_player.h"
#include "pipewire/wireplumber_mixer.h"
#include "render/animation/motion_service.h"
#include "render/backend/render_backend.h"
#include "render/core/texture_manager.h"
#include "render/text/font_weight_catalog.h"
#include "scripting/plugin_ipc.h"
#include "scripting/plugin_manifest.h"
#include "scripting/plugin_panel_shell.h"
#include "scripting/plugin_registry.h"
#include "scripting/plugin_runtime_context.h"
#include "shell/clipboard/clipboard_panel.h"
#include "shell/clipboard/clipboard_paste.h"
#include "shell/control_center/control_center_panel.h"
#include "shell/greeter/greeter_appearance_sync.h"
#include "shell/launcher/launcher_panel.h"
#include "shell/panel/plugin_panel.h"
#include "shell/polkit/polkit_panel.h"
#include "shell/session/session_ipc.h"
#include "shell/session/session_panel.h"
#include "shell/setup_wizard/setup_wizard_panel.h"
#include "shell/test/test_panel.h"
#include "shell/tooltip/tooltip_manager.h"
#include "shell/tray/tray_drawer_panel.h"
#include "shell/wallpaper/panel/wallpaper_panel.h"
#include "shell/wallpaper/wallpaper_paths.h"
#include "system/brightness_poll_source.h"
#include "system/brightness_service.h"
#include "system/distro_info.h"
#include "system/easyeffects_service.h"
#include "system/keyboard_backlight_service.h"
#include "system/system_monitor_service.h"
#include "ui/app_icon_colorization.h"
#include "ui/controls/input.h"
#include "ui/dialogs/color_picker_dialog.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/dialogs/glyph_picker_dialog.h"
#include "ui/style.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <malloc.h>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

std::atomic<bool> Application::s_shutdownRequested{false};

namespace {
  constexpr Logger kLog("app");

  float elapsedSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
  }

  template <typename Fn> void runStartupPhase(std::string_view label, Fn&& fn) {
    constexpr float kSlowStartupPhaseDebugMs = 50.0f;
    constexpr float kSlowStartupPhaseWarnMs = 1000.0f;

    const auto start = std::chrono::steady_clock::now();
    try {
      fn();
    } catch (...) {
      kLog.warn("startup phase {} failed after {:.1f}ms", label, elapsedSince(start));
      throw;
    }

    const float ms = elapsedSince(start);
    if (ms >= kSlowStartupPhaseWarnMs) {
      kLog.warn("startup phase {} took {:.1f}ms", label, ms);
    } else if (ms >= kSlowStartupPhaseDebugMs) {
      kLog.debug("startup phase {} took {:.1f}ms", label, ms);
    }
  }
} // namespace

Application::Application()
    : m_lockKeysService(m_wayland), m_gammaService(m_wayland), m_locationService(m_configService, m_httpClient),
      m_weatherService(m_configService, m_httpClient),
      m_calendarService(m_configService, m_httpClient, m_secretStore, &m_notificationManager) {
  m_notificationManager.loadPersistedHistory();
  notify::setInstance(&m_notificationManager);

  m_notificationManager.addEventCallback([this](const Notification& n, NotificationEvent event) {
    const char* kind = "updated";
    if (event == NotificationEvent::Added) {
      kind = "added";
    } else if (event == NotificationEvent::Closed) {
      kind = "closed";
    }
    const char* origin = (n.origin == NotificationOrigin::Internal) ? "internal" : "external";
    kLog.debug("notification {} id={} origin={}", kind, n.id, origin);

    if (event == NotificationEvent::Added && m_panelManager.isActivePanelContext("notifications")) {
      m_notificationManager.markNotificationHistorySeen();
    }

    // Keep bar widgets in sync with notification state changes.
    scheduleNotificationShellRefresh();
  });

  m_notificationManager.setStateCallback([this]() { scheduleNotificationShellRefresh(); });
}

Application::~Application() {
  TooltipManager::instance().shutdown();
  m_notificationManager.flushPersistedHistory();
  m_wayland.setClipboardService(nullptr);
  m_wayland.setTextInputService(nullptr);
  m_wayland.setVirtualKeyboardService(nullptr);
  notify::setInstance(nullptr);
}

void Application::run(std::function<void()> startupReadyCallback) {
  initLogFile();
  initLogLevelFromEnvironment();
  kLog.info("noctalia {}", noctalia::build_info::displayVersion());
  runStartupPhase("initServices", [this]() { initServices(); });
  runStartupPhase("initPlugins", [this]() {
    // Configure the plugin registry from [plugins] before any UI consumes it, and
    // re-apply on reload. Registered first so the registry updates ahead of bar /
    // control-center rebuilds when a plugin is enabled or disabled.
    m_pluginManager.refresh();
    m_configService.addReloadCallback([this]() { m_pluginManager.refresh(); });
    // Opt-in auto-update: pull each flagged git source in the background, once now
    // and then every 6h so a long-running session isn't stuck on the startup snapshot.
    runPluginAutoUpdate();
    m_pluginAutoUpdateTimer.startRepeating(std::chrono::hours(6), [this]() { runPluginAutoUpdate(); });
  });
  runStartupPhase("initUi", [this]() { initUi(); });
  runStartupPhase("initPluginServices", [this]() {
    // Outputs are enumerated by now (wallpaper created its surfaces in initUi); refresh
    // the script-visible output snapshot before any service/panel reads noctalia.outputs().
    if (m_syncScriptApiOutputs) {
      m_syncScriptApiOutputs();
    }
    m_pluginServiceHost.start(m_configService.config().plugins.pluginSettings);
    // Reconcile services when plugin settings change (start new, stop removed, re-seed
    // changed). Guarded by the plugins change flag so unrelated reloads don't churn.
    m_configService.addReloadCallback([this]() {
      if (m_configService.lastChange().plugins) {
        m_pluginServiceHost.refresh(m_configService.config().plugins.pluginSettings);
        reloadPluginLauncherProviders();
        reloadPluginPanels();
        m_settingsWindow.onPluginsChanged();
      }
    });
    // Plugins materialized after the bar was built (first-run clone/export) or a git
    // update() advance a source without a config change, so they bypass the config-reload
    // path. reload() rebuilds the bar widget tree against the now-populated registry —
    // refresh() only repaints, leaving newly available plugin widgets uncreated.
    m_pluginManager.setOnChanged([this]() {
      m_pluginServiceHost.refresh(m_configService.config().plugins.pluginSettings);
      m_bar.reload();
      reloadPluginLauncherProviders();
      reloadPluginPanels();
      m_settingsWindow.onPluginsChanged();
    });
    // A git-source enable exports on a worker thread; redraw the plugins list so the
    // row swaps between its spinner and toggle as the export starts and finishes.
    m_pluginManager.setOnEnablingChanged([this]() { m_settingsWindow.onPluginsChanged(); });
    m_pluginManager.setOnSourceUpdated([this](const std::string& sourceName) {
      m_settingsWindow.invalidatePluginSourceCache(sourceName);
    });
  });
  runStartupPhase("initIpc", [this]() { initIpc(); });
  runStartupPhase("buildPollSources", [this]() { (void)buildPollSources(); });

  runStartupPhase("startup hooks", [this]() {
    m_hookManager.reload(m_configService.config().hooks);
    m_hookManager.fire(HookKind::Started);
  });
  runStartupPhase("telemetry enqueue", [this]() {
    m_telemetryService.maybeSend(m_configService, m_httpClient, m_wayland);
  });

#ifdef __GLIBC__
  runStartupPhase("malloc_trim", []() { malloc_trim(0); });
#endif

  m_trayInitTimer.start(std::chrono::milliseconds(500), [this]() { startTrayService(); });
  m_polkitInitTimer.start(std::chrono::milliseconds(1000), [this]() { syncPolkitAgent(); });

  m_mainLoop = std::make_unique<MainLoop>(m_wayland, m_bar, [this]() { return currentPollSources(); });
  if (startupReadyCallback) {
    startupReadyCallback();
  }
  m_mainLoop->run();
  kLog.info("shutdown");
}
