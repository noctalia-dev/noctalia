#include "application.h"

#include "app/poll_source.h"
#include "compositors/compositor_detect.h"
#include "config/config_types.h"
#include "core/build_info.h"
#include "core/deferred_call.h"
#include "core/keybind_matcher.h"
#include "core/log.h"
#include "core/process.h"
#include "core/resource_paths.h"
#include "cursor-shape-v1-client-protocol.h"
#include "dbus/network/network_manager_service.h"
#include "dbus/network/wpa_supplicant_service.h"
#include "i18n/i18n.h"
#include "i18n/i18n_service.h"
#include "ipc/ipc_arg_parse.h"
#include "launcher/app_provider.h"
#include "launcher/emoji_provider.h"
#include "launcher/math_provider.h"
#include "launcher/plugin_launcher_provider.h"
#include "launcher/session_provider.h"
#include "launcher/wallpaper_provider.h"
#include "launcher/window_provider.h"
#include "notification/notifications.h"
#include "render/animation/motion_service.h"
#include "render/core/texture_manager.h"
#include "render/text/font_weight_catalog.h"
#include "scripting/plugin_manifest.h"
#include "scripting/plugin_registry.h"
#include "shell/clipboard/clipboard_panel.h"
#include "shell/clipboard/clipboard_paste.h"
#include "shell/control_center/control_center_panel.h"
#include "shell/greeter/greeter_appearance_sync.h"
#include "shell/launcher/launcher_panel.h"
#include "shell/session/session_ipc.h"
#include "shell/session/session_panel.h"
#include "shell/setup_wizard/setup_wizard_panel.h"
#include "shell/test/test_panel.h"
#include "shell/tooltip/tooltip_manager.h"
#include "shell/tray/tray_drawer_panel.h"
#include "shell/wallpaper/panel/wallpaper_panel.h"
#include "system/distro_info.h"
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
  constexpr bool kLockKeysEnabled = true;
  constexpr std::string_view kPolkitAuthorityBusName = "org.freedesktop.PolicyKit1";

  float elapsedSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
  }

  bool widgetIsLockKeys(std::string_view widgetName, const Config& config) {
    auto it = config.widgets.find(std::string(widgetName));
    if (it == config.widgets.end()) {
      return widgetName == "lock_keys";
    }
    return it->second.type == "lock_keys";
  }

  bool widgetListHasLockKeys(const std::vector<std::string>& widgets, const Config& config) {
    return std::ranges::any_of(widgets, [&config](const std::string& name) { return widgetIsLockKeys(name, config); });
  }

  std::string_view powerProfileOriginName(PowerProfilesChangeOrigin origin) {
    switch (origin) {
    case PowerProfilesChangeOrigin::Noctalia:
      return "noctalia";
    case PowerProfilesChangeOrigin::External:
      return "external";
    }
    return "external";
  }

  OsdContent powerProfileOsdContent(std::string_view profile) {
    return OsdContent{
        .kind = OsdKind::PowerProfile,
        .icon = std::string(profileGlyphName(profile)),
        .value = profileLabel(profile),
        .showProgress = false,
    };
  }

  OsdContent effectsProfileOsdContent(AudioEffectsProfileKind kind, std::string_view profile) {
    const char* labelKey = kind == AudioEffectsProfileKind::Input ? "osd.effects.input" : "osd.effects.output";
    return OsdContent{
        .icon = "adjustments",
        .value = i18n::tr(labelKey) + ": " + std::string(profile),
        .showProgress = false,
    };
  }

  OsdContent caffeineOsdContent(bool enabled) {
    return OsdContent{
        .kind = OsdKind::Caffeine,
        .icon = enabled ? "caffeine-on" : "caffeine-off",
        .value = i18n::tr(enabled ? "osd.caffeine.on" : "osd.caffeine.off"),
        .showProgress = false,
        .inactive = !enabled,
    };
  }

  OsdContent nightLightOsdContent(const GammaService& service) {
    std::string icon;
    std::string value;
    const bool inactive = !service.enabled() && !service.forceEnabled();
    if (service.forceEnabled()) {
      icon = "nightlight-forced";
      value = i18n::tr("osd.nightlight.forced");
    } else if (!service.enabled()) {
      icon = "nightlight-off";
      value = i18n::tr("osd.nightlight.off");
    } else if (service.active()) {
      icon = "nightlight-on";
      value = i18n::tr("osd.nightlight.running");
    } else {
      icon = "nightlight-on";
      value = i18n::tr("osd.nightlight.scheduled");
    }
    return OsdContent{
        .kind = OsdKind::NightLight,
        .icon = std::move(icon),
        .value = std::move(value),
        .showProgress = false,
        .inactive = inactive,
    };
  }

  OsdContent dndOsdContent(bool enabled) {
    return OsdContent{
        .kind = OsdKind::Dnd,
        .icon = enabled ? "bell-off" : "bell",
        .value = i18n::tr(enabled ? "osd.dnd.on" : "osd.dnd.off"),
        .showProgress = false,
        .inactive = !enabled,
    };
  }

  OsdContent wifiOsdContent(bool enabled) {
    return OsdContent{
        .kind = OsdKind::Wifi,
        .icon = enabled ? "wifi" : "wifi-off",
        .value = i18n::tr(enabled ? "osd.wifi.on" : "osd.wifi.off"),
        .showProgress = false,
        .inactive = !enabled,
    };
  }

  OsdContent bluetoothOsdContent(bool enabled) {
    return OsdContent{
        .kind = OsdKind::Bluetooth,
        .icon = enabled ? "bluetooth" : "bluetooth-off",
        .value = i18n::tr(enabled ? "osd.bluetooth.on" : "osd.bluetooth.off"),
        .showProgress = false,
        .inactive = !enabled,
    };
  }

  bool barMayRender(const BarConfig& bar) {
    if (bar.enabled) {
      return true;
    }
    return std::ranges::any_of(bar.monitorOverrides, [](const BarMonitorOverride& ovr) {
      return ovr.enabled.value_or(false);
    });
  }

  bool configHasLockKeysWidget(const Config& config) {
    return std::ranges::any_of(config.bars, [&config](const BarConfig& bar) {
      return barMayRender(bar)
          && (widgetListHasLockKeys(bar.startWidgets, config)
              || widgetListHasLockKeys(bar.centerWidgets, config)
              || widgetListHasLockKeys(bar.endWidgets, config));
    });
  }

  bool lockKeysConsumersEnabled(const Config& config) {
    return config.osd.kinds.lockKeys || configHasLockKeysWidget(config);
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

  void signal_handler(int signum) {
    if (signum == SIGTERM || signum == SIGINT) {
      Application::s_shutdownRequested = true;
    }
  }

} // namespace

Application::Application()
    : m_lockKeysService(m_wayland), m_gammaService(m_wayland), m_locationService(m_configService, m_httpClient),
      m_weatherService(m_configService, m_httpClient),
      m_calendarService(m_configService, m_httpClient, &m_notificationManager) {
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

void Application::scheduleNotificationShellRefresh() {
  if (m_notificationShellRefreshScheduled) {
    return;
  }
  m_notificationShellRefreshScheduled = true;
  DeferredCall::callLater([this]() {
    m_notificationShellRefreshScheduled = false;
    m_bar.refresh();
    if (m_panelManager.isOpenPanel("control-center")) {
      m_panelManager.refresh();
    }
  });
}

Application::~Application() {
  TooltipManager::instance().shutdown();
  m_notificationManager.flushPersistedHistory();
  m_wayland.setClipboardService(nullptr);
  m_wayland.setTextInputService(nullptr);
  m_wayland.setVirtualKeyboardService(nullptr);
  notify::setInstance(nullptr);
}

void Application::syncNotificationDaemon() {
  if (m_bus == nullptr) {
    m_notificationPollSource.setDbusService(nullptr);
    m_notificationDbus.reset();
    m_notificationDaemonEnabled.reset();
    m_notificationDaemonInitFailed = false;
    return;
  }

  const bool enabled = m_configService.config().notification.enableDaemon;
  const bool enabledChanged = !m_notificationDaemonEnabled.has_value() || *m_notificationDaemonEnabled != enabled;
  m_notificationDaemonEnabled = enabled;

  if (!enabled) {
    if (m_notificationDbus != nullptr) {
      kLog.info("notification daemon disabled by config");
    }
    m_notificationPollSource.setDbusService(nullptr);
    m_notificationDbus.reset();
    m_notificationDaemonInitFailed = false;
    return;
  }

  if (m_notificationDbus != nullptr) {
    if (m_notificationDbus->isHealthy()) {
      m_notificationDaemonInitFailed = false;
      return;
    }
    kLog.info("notification daemon connection lost; re-registering");
    m_notificationPollSource.setDbusService(nullptr);
    m_notificationDbus.reset();
  }

  if (m_notificationDaemonInitFailed && !enabledChanged) {
    return;
  }

  try {
    m_notificationDbus = std::make_unique<NotificationService>(*m_bus, m_notificationManager);
    m_notificationPollSource.setDbusService(m_notificationDbus.get());
    m_notificationDaemonInitFailed = false;
    kLog.info("listening on org.freedesktop.Notifications");
  } catch (const std::exception& ownerError) {
    if (compositors::isKde()) {
      try {
        m_notificationDbus = std::make_unique<KdeNotificationClient>(*m_bus, m_notificationManager);
        m_notificationPollSource.setDbusService(m_notificationDbus.get());
        m_notificationDaemonInitFailed = false;
        kLog.info("listening on KDE Plasma notifications via NotificationWatcher");
        return;
      } catch (const std::exception& kdeError) {
        kLog.warn("notifications disabled: {}", kdeError.what());
        m_notificationDbus.reset();
        m_notificationPollSource.setDbusService(nullptr);
        m_notificationDaemonInitFailed = true;
        m_notificationManager.addInternal(
            "Noctalia", i18n::tr("notifications.internal.dbus-disabled"), kdeError.what(), Urgency::Low
        );
        return;
      }
    }

    kLog.warn("notifications disabled: {}", ownerError.what());
    m_notificationDbus.reset();
    m_notificationPollSource.setDbusService(nullptr);
    m_notificationDaemonInitFailed = true;
    m_notificationManager.addInternal(
        "Noctalia", i18n::tr("notifications.internal.dbus-disabled"), ownerError.what(), Urgency::Low
    );
  }
}

void Application::installNotificationBusNameWatch() {
  if (m_notificationBusNameWatchInstalled || m_bus == nullptr) {
    return;
  }

  m_notificationBusNameWatchProxy = sdbus::createProxy(
      m_bus->connection(), sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"}
  );
  m_notificationBusNameWatchProxy->uponSignal("NameOwnerChanged")
      .onInterface("org.freedesktop.DBus")
      .call([this](const std::string& name, const std::string& /*oldOwner*/, const std::string& /*newOwner*/) {
        if (name != notification_dbus::kFreedesktopNotificationsBusName) {
          return;
        }
        DeferredCall::callLater([this]() {
          m_notificationDaemonInitFailed = false;
          syncNotificationDaemon();
        });
      });
  m_notificationBusNameWatchInstalled = true;
}

void Application::syncPolkitAgent() {
  if (m_systemBus == nullptr) {
    m_polkitPollSource.reset();
    m_polkitAgent.reset();
    return;
  }

  if (!m_configService.config().shell.polkitAgent) {
    if (m_polkitAgent != nullptr) {
      kLog.info("polkit agent disabled by config");
    }
    m_polkitPollSource.reset();
    m_polkitAgent.reset();
    return;
  }

  if (m_polkitAgent != nullptr) {
    return;
  }

  try {
    if (!m_systemBus->nameHasOwner(kPolkitAuthorityBusName)) {
      kLog.warn("polkit agent disabled: {} is not running", kPolkitAuthorityBusName);
      m_polkitPollSource.reset();
      m_polkitAgent.reset();
      return;
    }
  } catch (const std::exception& e) {
    kLog.warn("polkit agent disabled: failed to query {} owner: {}", kPolkitAuthorityBusName, e.what());
    m_polkitPollSource.reset();
    m_polkitAgent.reset();
    return;
  }

  m_polkitAgent = std::make_unique<PolkitAgent>(*m_systemBus);
  m_polkitAgent->setReadyCallback([this](bool ok, const std::string& error) {
    if (!ok) {
      kLog.warn("polkit agent disabled: {}", error);
      DeferredCall::callLater([this]() {
        m_polkitPollSource.reset();
        m_polkitAgent.reset();
      });
      return;
    }
    kLog.info("polkit authentication agent active");
  });
  m_polkitAgent->setStateCallback([this]() {
    if (m_polkitAgent == nullptr) {
      return;
    }
    if (!m_polkitAgent->hasPendingRequest()) {
      if (m_panelManager.isOpenPanel("polkit")) {
        m_panelManager.close();
      }
      return;
    }
    if (!m_panelManager.isOpenPanel("polkit")) {
      wl_output* output = m_compositorPlatform.preferredInteractiveOutput(std::chrono::milliseconds(1200));
      m_panelManager.openPanel("polkit", PanelOpenRequest{.output = output});
    } else {
      m_panelManager.refresh();
    }
  });
  m_polkitPollSource = std::make_unique<PolkitPollSource>(*m_polkitAgent);
  m_polkitAgent->start();
}

void Application::syncScreenTimeService() {
  m_screenTimeService.setEnabled(m_configService.config().shell.screenTimeEnabled);
}

void Application::syncClipboardService() {
  const bool enabled = m_configService.config().shell.clipboardEnabled;
  const auto shouldRefreshControlCenter = [this]() { return m_panelManager.isOpenPanel("control-center"); };

  // The live clipboard transport (read current selection + set selection)
  // stays active regardless of config so basic copy/paste keeps working in
  // every text field. The toggle only controls history retention/persistence
  // and the history UI.
  m_wayland.setClipboardService(&m_clipboardService);
  Input::setTextClipboard(&m_clipboardService);
  m_clipboardService.setHistoryRetentionEnabled(enabled);
  m_clipboardService.setMaxHistoryEntries(
      static_cast<std::size_t>(m_configService.config().shell.clipboardHistoryMaxEntries)
  );

  if (!enabled) {
    if (m_panelManager.isOpenPanel("clipboard")) {
      m_panelManager.close();
    }
    kLog.info("clipboard history disabled by config (live copy/paste still active)");
  }

  m_bar.refresh();
  if (shouldRefreshControlCenter()) {
    m_panelManager.refresh();
  }
}

void Application::run(std::function<void()> startupReadyCallback) {
  initLogFile();
  kLog.info("noctalia {}", noctalia::build_info::displayVersion());
  runStartupPhase("initServices", [this]() { initServices(); });
  runStartupPhase("initPlugins", [this]() {
    // Configure the plugin registry from [plugins] before any UI consumes it, and
    // re-apply on reload. Registered first so the registry updates ahead of bar /
    // control-center rebuilds when a plugin is enabled or disabled.
    m_pluginManager.refresh();
    m_configService.addReloadCallback([this]() { m_pluginManager.refresh(); });
    // Opt-in auto-update: pull each flagged git source in the background.
    for (const auto& source : m_configService.config().plugins.sources) {
      if (source.kind == PluginSourceKind::Git && source.autoUpdate) {
        m_pluginManager.update(source.name);
      }
    }
  });
  runStartupPhase("initUi", [this]() { initUi(); });
  runStartupPhase("initPluginServices", [this]() {
    m_pluginServiceHost.start(m_configService.config().plugins.pluginSettings);
    // Reconcile services when plugin settings change (start new, stop removed, re-seed
    // changed). Guarded by the plugins change flag so unrelated reloads don't churn.
    m_configService.addReloadCallback([this]() {
      if (m_configService.lastChange().plugins) {
        m_pluginServiceHost.refresh(m_configService.config().plugins.pluginSettings);
        reloadPluginLauncherProviders();
        m_settingsWindow.onPluginsChanged();
      }
    });
    // A git update() advances a source without a config change, so it bypasses the
    // reload path: rebuild the bar and reconcile services for the new revision.
    m_pluginManager.setOnChanged([this]() {
      m_pluginServiceHost.refresh(m_configService.config().plugins.pluginSettings);
      m_bar.refresh();
      reloadPluginLauncherProviders();
      m_settingsWindow.onPluginsChanged();
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

void Application::initServices() {
  std::signal(SIGTERM, signal_handler);
  std::signal(SIGINT, signal_handler);

  auto shouldRefreshControlCenter = [this]() { return m_panelManager.isOpenPanel("control-center"); };

  auto applyMotionConfig = [this]() {
    auto& motion = MotionService::instance();
    motion.setSpeed(m_configService.config().shell.animation.speed);
    motion.setEnabled(m_configService.config().shell.animation.enabled);
  };
  auto applyStyleConfig = [this, lastCornerRadiusScale = std::numeric_limits<float>::quiet_NaN()]() mutable {
    const float corner = m_configService.config().shell.cornerRadiusScale;
    const bool cornerChanged =
        std::isfinite(lastCornerRadiusScale) && std::abs(corner - lastCornerRadiusScale) > 1.0e-4f;
    Style::setCornerRadiusScale(corner);
    lastCornerRadiusScale = corner;
    if (cornerChanged) {
      m_notificationToast.requestLayout();
      m_panelManager.requestLayout();
    }
  };
  auto applyPasswordMaskStyle = [this]() {
    const auto style = m_configService.config().shell.passwordMaskStyle == PasswordMaskStyle::RandomIcons
        ? Input::PasswordMaskStyle::RandomIcons
        : Input::PasswordMaskStyle::CircleFilled;
    Input::setPasswordMaskStyle(style);
  };
  applyMotionConfig();
  applyStyleConfig();
  applyPasswordMaskStyle();
  m_httpClient.setOfflineMode(m_configService.config().shell.offlineMode);
  m_configService.addReloadCallback(applyMotionConfig);
  m_configService.addReloadCallback(applyStyleConfig);
  m_configService.addReloadCallback(applyPasswordMaskStyle);
  m_configService.addReloadCallback([this]() {
    m_httpClient.setOfflineMode(m_configService.config().shell.offlineMode);
  });
  m_configService.addReloadCallback([this]() { syncClipboardService(); });
  m_configService.addReloadCallback([this]() { syncScreenTimeService(); });
  m_communityPaletteService.setReadyCallback([this]() {
    // A refreshed catalog may carry a new md5 for the selected palette; re-resolve
    // so a stale cached palette gets re-downloaded and faded in.
    m_themeService.onConfigReload();
    m_settingsWindow.onExternalOptionsChanged();
  });
  m_communityPaletteService.sync();
  m_configService.addReloadCallback(
      [this]() {
        if (m_configService.lastChange().theme) {
          m_communityPaletteService.sync();
        }
      },
      "community-palette-sync"
  );
  m_communityTemplateService.setReadyCallback([this]() {
    if (m_configService.config().theme.templates.enableCommunityTemplates) {
      m_themeService.onConfigReload();
      m_settingsWindow.onExternalOptionsChanged();
    }
  });
  m_communityTemplateService.sync(m_configService.config().theme.templates);
  m_configService.addReloadCallback(
      [this]() {
        if (m_configService.lastChange().theme) {
          m_communityTemplateService.sync(m_configService.config().theme.templates);
        }
      },
      "community-template-sync"
  );

  // i18n has no dependencies on other services and must be ready before any
  // UI construction reads a translated string.
  i18n::Service::instance().init(m_configService.config().shell.lang);
  m_configService.addReloadCallback([this]() {
    i18n::Service::instance().setLanguage(m_configService.config().shell.lang);
  });

  // Apply theme before any UI constructs palette-dependent scene nodes.
  m_themeService.setResolvedCallback([this, lastResolvedThemeMode = std::optional<std::string>{}](
                                         const noctalia::theme::GeneratedPalette& generated, std::string_view mode
                                     ) mutable {
    const std::string resolvedMode(mode);
    const std::string configuredMode(enumToKey(kThemeModes, m_themeService.configuredMode()));
    m_scriptApi.setDarkMode(resolvedMode != "light");
    m_templateApplyService.apply(generated, mode);
    m_hookManager.fire(HookKind::ColorsChanged);
    if (lastResolvedThemeMode.has_value() && *lastResolvedThemeMode != resolvedMode) {
      m_hookManager.fire(
          HookKind::ThemeModeChanged,
          {{"NOCTALIA_THEME_MODE", resolvedMode},
           {"NOCTALIA_THEME_MODE_PREVIOUS", *lastResolvedThemeMode},
           {"NOCTALIA_THEME_MODE_CONFIGURED", configuredMode}}
      );
    }
    lastResolvedThemeMode = resolvedMode;
  });
  m_themeService.apply();
  m_configService.addReloadCallback([this]() { m_themeService.onConfigReload(); }, "theme");
  {
    static ShellAppIconColorizationSettings lastAppIconColorization =
        shellAppIconColorizationSettings(m_configService.config().shell);
    m_configService.addReloadCallback(
        [this]() {
          const auto current = shellAppIconColorizationSettings(m_configService.config().shell);
          if (current == lastAppIconColorization) {
            return;
          }
          lastAppIconColorization = current;
          notifyShellAppIconColorizationChanged();
        },
        "app-icon-colorization"
    );
  }

  if (!m_wayland.connect()) {
    throw std::runtime_error("failed to connect to Wayland display");
  }
  m_compositorPlatform.initialize();
  m_screenTimeService.initialize(&m_wayland);
  syncScreenTimeService();
  m_screenTimeService.setChangeCallback([this]() {
    if (m_panelManager.isOpenPanel("control-center")) {
      m_panelManager.refresh();
    }
  });
  if (m_configService.config().shell.disableMipmaps) {
    TextureManager::setGlobalMipmapsEnabled(false);
  }
  m_glShared.initialize(m_wayland.display(), m_configService.config().shell.sharedGlContext);
  auto* sharedGlPtr = m_glShared.hasSharedContext() ? &m_glShared : nullptr;
  m_sharedTextureCache.initialize(sharedGlPtr);
  m_asyncTextureCache.initialize(sharedGlPtr);
  m_wayland.setTextInputService(&m_textInputService);
  m_wayland.setVirtualKeyboardService(&m_virtualKeyboardService);

  auto bindKeybind = [this](KeybindAction action) {
    return [this, action](std::uint32_t sym, std::uint32_t modifiers) {
      return m_configService.matchesKeybind(action, sym, modifiers);
    };
  };
  KeybindMatcher::setMatcher(KeybindAction::Validate, bindKeybind(KeybindAction::Validate));
  KeybindMatcher::setMatcher(KeybindAction::Cancel, bindKeybind(KeybindAction::Cancel));
  KeybindMatcher::setMatcher(KeybindAction::Left, bindKeybind(KeybindAction::Left));
  KeybindMatcher::setMatcher(KeybindAction::Right, bindKeybind(KeybindAction::Right));
  KeybindMatcher::setMatcher(KeybindAction::Up, bindKeybind(KeybindAction::Up));
  KeybindMatcher::setMatcher(KeybindAction::Down, bindKeybind(KeybindAction::Down));

  Input::setValidateKeyMatcher([this](std::uint32_t sym, std::uint32_t modifiers) {
    return m_configService.matchesKeybind(KeybindAction::Validate, sym, modifiers);
  });

  m_wayland.setOutputChangeCallback([this]() {
    if (m_brightnessService != nullptr) {
      m_brightnessService->onOutputsChanged();
    }
    m_gammaService.onOutputsChanged();
    m_wallpaper.onOutputChange();
    m_backdrop.onOutputChange();
    m_bar.onOutputChange();
    m_dock.onOutputChange();
    m_desktopWidgetsController.onOutputChange();
    m_lockscreenWidgetsController.onOutputChange();
    m_screenCorners.onOutputChange();
    m_lockScreen.onOutputChange();
    m_idleGraceOverlay.onOutputChange();
    m_idleInhibitor.onOutputChange();
    m_overviewLauncherCapture.onOutputChange();
    m_screenshotService.onOutputChange();
    m_notificationToast.onOutputChange();
    m_osdOverlay.onOutputChange();
    m_windowSwitcher.onOutputChange();
  });
  m_clipboardService.setChangeCallback([this]() {
    if (m_panelManager.isOpenPanel("clipboard")) {
      m_panelManager.refresh();
    }
  });
  m_compositorPlatform.setWorkspaceChangeCallback([this]() {
    m_bar.refresh();
    m_windowSwitcher.onToplevelChange();
  });
  m_compositorPlatform.setKeyboardLayoutChangeCallback([this]() {
    m_bar.refresh();
    if (m_configService.config().osd.kinds.keyboardLayout) {
      m_keyboardLayoutOsd.onLayoutChanged(m_compositorPlatform, m_configService.config());
    }
  });
  m_compositorPlatform.setToplevelChangeCallback([this]() {
    m_screenTimeService.onFocusChange();
    m_bar.refresh();
    m_dock.refresh();
    m_windowSwitcher.onToplevelChange();
    if (m_panelManager.isOpenPanel("control-center")) {
      m_panelManager.refresh();
    }
    if (!m_lockScreen.isActive() && m_wayland.hasPointerPosition() && !m_wayland.activeToplevel().has_value()) {
      const std::uint32_t serial = m_wayland.lastInputSerial();
      if (serial != 0) {
        m_wayland.setCursorShape(serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
      }
    }
  });
  if constexpr (kLockKeysEnabled) {
    if (lockKeysConsumersEnabled(m_configService.config())) {
      m_lockKeysService.refreshNow();
    }
    m_lockKeysService.setChangeCallback(
        [this](const WaylandSeat::LockKeysState& previous, const WaylandSeat::LockKeysState& current) {
          const Config& config = m_configService.config();
          if (config.osd.kinds.lockKeys) {
            m_lockKeysOsd.onLockKeysChanged(previous, current);
          }
          if (configHasLockKeysWidget(config)) {
            m_bar.refresh();
          }
        }
    );
  }
  m_idleInhibitor.initialize(m_wayland);
  m_idleInhibitor.setChangeCallback([this, shouldRefreshControlCenter]() {
    if (m_configService.config().osd.kinds.caffeine) {
      m_osdOverlay.show(caffeineOsdContent(m_idleInhibitor.enabled()));
    }
    m_bar.refresh();
    if (shouldRefreshControlCenter()) {
      m_panelManager.refresh();
    }
  });
  m_hookManager.setCommandRunner([this](const std::string& command) { return runUserCommand(command); });
  m_hookManager.setBlockingCommandRunner([this](const std::string& command) {
    return runUserCommandBlocking(command);
  });
  m_hookManager.reload(m_configService.config().hooks);
  m_configService.addReloadCallback(
      [this]() {
        if (m_configService.lastChange().hooks) {
          m_hookManager.reload(m_configService.config().hooks);
        }
      },
      "hooks"
  );
  m_gammaService.setLocationResolving(m_locationService.resolving());
  m_gammaService.reload(m_configService.config().nightlight, m_configService.config().location);
  m_gammaService.setChangeCallback([this, shouldRefreshControlCenter]() {
    m_bar.refresh();
    if (shouldRefreshControlCenter()) {
      m_panelManager.refresh();
    }
  });
  m_gammaService.setStateFeedbackCallback([this]() {
    if (m_configService.config().osd.kinds.nightlight) {
      m_osdOverlay.show(nightLightOsdContent(m_gammaService));
    }
  });
  m_configService.addReloadCallback([this]() {
    m_gammaService.reload(m_configService.config().nightlight, m_configService.config().location);
  });

  // Register all wallpaper consumers in the single-callback slot.
  m_configService.setWallpaperChangeCallback([this]() {
    m_wallpaper.onStateChange();
    m_backdrop.onStateChange();
    m_lockScreen.onWallpaperChanged();
    m_themeService.onWallpaperChange();
    if (m_panelManager.isOpenPanel("control-center")) {
      m_panelManager.refresh();
    }
    m_hookManager.fire(HookKind::WallpaperChanged);
  });

  m_themeService.setChangeCallback([this]() {
    requestAllSurfacesRedraw();
    m_lockScreen.onThemeChanged();
    m_trayMenu.onThemeChanged();
    m_backdrop.onThemeChanged();
    m_settingsWindow.onThemeChanged();
  });

  if (const auto distro = DistroDetector::detect(); distro.has_value()) {
    const auto& label = !distro->prettyName.empty() ? distro->prettyName
        : !distro->name.empty()                     ? distro->name
                                                    : distro->id;
    kLog.info("distro: {}", label);
  } else {
    kLog.info("distro: unknown");
  }

  try {
    m_systemMonitor = std::make_unique<SystemMonitorService>(m_configService.config().system.monitor);
    if (m_systemMonitor->isRunning()) {
      kLog.info("system monitor service active");
    } else {
      kLog.info("system monitor service disabled by config");
    }
    m_configService.addReloadCallback([this, shouldRefreshControlCenter]() {
      if (m_systemMonitor == nullptr) {
        return;
      }

      const bool wasRunning = m_systemMonitor->isRunning();
      try {
        m_systemMonitor->applyConfig(m_configService.config().system.monitor);
      } catch (const std::exception& e) {
        kLog.warn("system monitor service failed to start: {}", e.what());
        return;
      }

      if (wasRunning != m_systemMonitor->isRunning()) {
        kLog.info("system monitor service {}", m_systemMonitor->isRunning() ? "active" : "disabled by config");
      }
      m_bar.refresh();
      m_desktopWidgetsController.requestLayout();
      if (shouldRefreshControlCenter()) {
        m_panelManager.refresh();
      }
    });
  } catch (const std::exception& e) {
    kLog.warn("system monitor service disabled: {}", e.what());
    m_systemMonitor.reset();
  }

  try {
    m_systemBus = std::make_unique<SystemBus>();
    kLog.info("connected to system bus");
  } catch (const std::exception& e) {
    kLog.warn("system dbus disabled: {}", e.what());
    m_systemBus.reset();
  }

  if (m_systemBus != nullptr) {
    if (m_systemBus->nameHasOwner("org.freedesktop.login1")) {
      try {
        m_logindService = std::make_unique<LogindService>(*m_systemBus);
        m_logindService->setPrepareForSleepCallback([this](bool sleeping) {
          if (sleeping) {
            return;
          }
          kLog.info("system resumed; rechecking night light schedule");
          m_gammaService.reevaluateSchedule();
          // BlueZ property-change signals can be missed across the suspend window, leaving our
          // cached adapter state stale. Re-sync now and again shortly after, since BlueZ may take a
          // moment to restore the adapter on resume.
          if (m_bluetoothService != nullptr) {
            m_bluetoothService->refresh();
            m_bluetoothResumeTimer.start(std::chrono::seconds(2), [this]() {
              if (m_bluetoothService != nullptr) {
                m_bluetoothService->refresh();
              }
            });
          }
        });
        kLog.info("logind sleep monitor active");
        m_idleInhibitor.setLogindService(m_logindService.get());
      } catch (const std::exception& e) {
        kLog.warn("logind sleep monitor disabled: {}", e.what());
        m_logindService.reset();
      }
    } else {
      kLog.info("logind not available on system bus; sleep monitor disabled");
    }

    try {
      m_accountsService = std::make_unique<AccountsService>(*m_systemBus);
      m_accountsService->setChangeCallback([this]() {
        m_panelManager.refresh();
        m_settingsWindow.onExternalOptionsChanged();
      });
      kLog.info("accounts service active for uid {}", m_accountsService->sessionUid());
    } catch (const std::exception& e) {
      kLog.info("accounts service disabled: {}", e.what());
      m_accountsService.reset();
    }

    try {
      m_powerProfilesService = std::make_unique<PowerProfilesService>(*m_systemBus);
      m_powerProfilesService->setChangeCallback(
          [this, shouldRefreshControlCenter](const PowerProfilesState& state, PowerProfilesChangeOrigin origin) {
            m_bar.refresh();
            if (shouldRefreshControlCenter()) {
              m_panelManager.refresh();
            }

            const std::string& active = state.activeProfile;
            if (active.empty()) {
              return;
            }
            onPowerProfileChangedForEvents(state, origin);
          }
      );
      if (m_powerProfilesService->hasStateSnapshot() && !m_powerProfilesService->activeProfile().empty()) {
        m_prevPowerProfileActiveForEvents = m_powerProfilesService->activeProfile();
        kLog.info("power profiles active profile: {}", m_powerProfilesService->activeProfile());
      } else if (!m_powerProfilesService->hasStateSnapshot()) {
        kLog.info("power profiles service active (state loading asynchronously)");
      } else {
        kLog.info("power profiles service active");
      }
    } catch (const std::exception& e) {
      kLog.warn("power profiles disabled: {}", e.what());
      m_powerProfilesService.reset();
    }

    try {
      m_upowerService = std::make_unique<UPowerService>(*m_systemBus);
      m_batteryHookState.reset(m_upowerService->state());
      m_batteryWarningMonitor.evaluate(m_configService.config().battery, *m_upowerService, m_notificationManager);
      m_upowerService->setChangeCallback([this, shouldRefreshControlCenter]() {
        onUpowerStateChangedForHooks();
        m_batteryWarningMonitor.evaluate(m_configService.config().battery, *m_upowerService, m_notificationManager);
        m_bar.refresh();
        m_settingsWindow.onExternalOptionsChanged();
        if (shouldRefreshControlCenter()) {
          m_panelManager.refresh();
        }
      });
      m_configService.addReloadCallback(
          [this]() {
            if (m_configService.lastChange().battery && m_upowerService != nullptr) {
              m_batteryWarningMonitor.evaluate(
                  m_configService.config().battery, *m_upowerService, m_notificationManager
              );
              m_bar.refresh();
            }
          },
          "battery"
      );
    } catch (const std::exception& e) {
      kLog.warn("upower disabled: {}", e.what());
      m_upowerService.reset();
    }

    try {
      m_networkService = std::make_unique<NetworkManagerService>(*m_systemBus);
      m_networkService->setChangeCallback(
          [this, shouldRefreshControlCenter](const NetworkState& state, NetworkChangeOrigin origin) {
            onNetworkStateChangedForEvents(state, origin);
            m_bar.refresh();
            if (shouldRefreshControlCenter()) {
              m_panelManager.refresh();
            }
          }
      );
      if (m_networkService->hasStateSnapshot()) {
        m_prevWirelessEnabledForEvents = m_networkService->state().wirelessEnabled;
      }
      kLog.info("network service active");
    } catch (const std::exception& e) {
      kLog.warn("NetworkManager unavailable ({}), trying wpa_supplicant", e.what());
      try {
        m_networkService = std::make_unique<WpaSupplicantService>(*m_systemBus);
        m_networkService->setChangeCallback(
            [this, shouldRefreshControlCenter](const NetworkState& state, NetworkChangeOrigin origin) {
              onNetworkStateChangedForEvents(state, origin);
              m_bar.refresh();
              if (shouldRefreshControlCenter()) {
                m_panelManager.refresh();
              }
            }
        );
        if (m_networkService->hasStateSnapshot()) {
          m_prevWirelessEnabledForEvents = m_networkService->state().wirelessEnabled;
        }
        kLog.info("network service active (wpa_supplicant)");
      } catch (const std::exception& e2) {
        kLog.warn("network service disabled: {}", e2.what());
        m_networkService.reset();
      }
    }

    if (m_networkService != nullptr && m_networkService->supportsSecretAgent()) {
      try {
        m_networkSecretAgent = std::make_unique<NetworkSecretAgent>(*m_systemBus);
      } catch (const std::exception& e) {
        kLog.warn("network secret agent disabled: {}", e.what());
        m_networkSecretAgent.reset();
      }
    }

    try {
      m_bluetoothService = std::make_unique<BluetoothService>(*m_systemBus);
      auto refreshBluetoothUi = [this, shouldRefreshControlCenter]() {
        m_bar.refresh();
        if (shouldRefreshControlCenter()) {
          m_panelManager.refresh();
        }
      };
      m_bluetoothService->setStateCallback(
          [this, refreshBluetoothUi](const BluetoothState& state, BluetoothStateChangeOrigin origin) {
            onBluetoothStateChangedForEvents(state, origin);
            refreshBluetoothUi();
          }
      );
      m_bluetoothService->setDevicesCallback([refreshBluetoothUi](const std::vector<BluetoothDeviceInfo>& /*devices*/) {
        refreshBluetoothUi();
      });
      if (m_bluetoothService->hasStateSnapshot()) {
        m_prevBluetoothPoweredForEvents = m_bluetoothService->state().powered;
      }
      kLog.info("bluetooth service active");
    } catch (const std::exception& e) {
      kLog.warn("bluetooth service disabled: {}", e.what());
      m_bluetoothService.reset();
    }

    if (m_bluetoothService != nullptr) {
      try {
        m_bluetoothAgent = std::make_unique<BluetoothAgent>(*m_systemBus);
        m_bluetoothAgent->setRequestCallback([this,
                                              shouldRefreshControlCenter](const BluetoothPairingRequest& /*request*/) {
          if (shouldRefreshControlCenter()) {
            m_panelManager.refresh();
          }
        });
      } catch (const std::exception& e) {
        kLog.warn("bluetooth agent disabled: {}", e.what());
        m_bluetoothAgent.reset();
      }
    }

    m_configService.addReloadCallback([this]() { syncPolkitAgent(); });
  }

  try {
    m_brightnessService = std::make_unique<BrightnessService>(
        m_systemBus.get(), m_compositorPlatform, m_configService.config().brightness
    );
    m_brightnessService->setChangeCallback([this, shouldRefreshControlCenter]() {
      m_brightnessOsd.onBrightnessChanged(*m_brightnessService);
      m_bar.refresh();
      if (shouldRefreshControlCenter()) {
        m_panelManager.refresh();
      }
    });
    m_configService.addReloadCallback(
        [this, shouldRefreshControlCenter]() {
          if (m_brightnessService == nullptr || !m_configService.lastChange().brightness) {
            return;
          }
          m_brightnessService->reload(m_configService.config().brightness);
          m_bar.refresh();
          if (shouldRefreshControlCenter()) {
            m_panelManager.refresh();
          }
        },
        "brightness"
    );
  } catch (const std::exception& e) {
    kLog.warn("brightness service disabled: {}", e.what());
    m_brightnessService.reset();
  }

  try {
    m_pipewireService = std::make_unique<PipeWireService>();
    m_easyEffectsService = std::make_unique<EasyEffectsService>();
    m_easyEffectsService->refreshProfiles();
    m_easyEffectsService->refreshActiveEffectsProfiles();
    m_pipewireSpectrum = std::make_unique<PipeWireSpectrum>(*m_pipewireService);
    m_soundPlayer = std::make_unique<SoundPlayer>(m_pipewireService->loop());

    struct LoadedSoundPaths {
      std::filesystem::path volumeChange;
      std::filesystem::path notification;
    };
    auto loadedSoundPaths = std::make_shared<LoadedSoundPaths>();

    auto applySoundConfig = [this, loadedSoundPaths]() {
      if (m_soundPlayer == nullptr) {
        return;
      }

      const auto& audio = m_configService.config().audio;
      m_soundPlayer->setVolume(audio.enableSounds ? audio.soundVolume : 0.0f);

      auto resolveSoundPath = [](const std::string& configured, std::string_view bundledRelative) {
        if (configured.empty()) {
          return paths::assetPath(bundledRelative);
        }
        const std::filesystem::path expanded = FileUtils::expandUserPath(configured);
        if (expanded.is_absolute()) {
          return expanded;
        }
        return paths::assetPath(expanded.string());
      };

      const auto volumeChangePath = resolveSoundPath(audio.volumeChangeSound, "sounds/volume-change.wav");
      if (loadedSoundPaths->volumeChange != volumeChangePath) {
        if (m_soundPlayer->load("volume-change", volumeChangePath)) {
          loadedSoundPaths->volumeChange = volumeChangePath;
        }
      }

      const auto notificationPath = resolveSoundPath(audio.notificationSound, "sounds/notification.wav");
      if (loadedSoundPaths->notification != notificationPath) {
        if (m_soundPlayer->load("notification", notificationPath)) {
          loadedSoundPaths->notification = notificationPath;
        }
      }
    };
    applySoundConfig();
    m_configService.addReloadCallback(
        [this, applySoundConfig]() {
          if (m_configService.lastChange().audio) {
            applySoundConfig();
          }
        },
        "sound"
    );
  } catch (const std::exception& e) {
    kLog.warn("pipewire disabled: {}", e.what());
    m_soundPlayer.reset();
    m_pipewireSpectrum.reset();
    m_easyEffectsService.reset();
    m_pipewireService.reset();
  }

  try {
    m_bus = std::make_unique<SessionBus>();
    kLog.info("connected to session bus");
  } catch (const std::exception& e) {
    kLog.warn("dbus disabled: {}", e.what());
    m_notificationManager.addInternal(
        "Noctalia", i18n::tr("notifications.internal.session-bus-unavailable"), e.what(), Urgency::Low
    );
  }

  if (m_bus != nullptr) {
    try {
      m_debugService = std::make_unique<DebugService>(*m_bus, m_notificationManager);
      kLog.info("debug service active on dev.noctalia.Debug");
    } catch (const std::exception& e) {
      kLog.warn("debug service disabled: {}", e.what());
      m_debugService.reset();
    }

    try {
      m_mprisService = std::make_unique<MprisService>(*m_bus);
      auto applyMprisConfig = [this]() {
        if (m_mprisService == nullptr) {
          return;
        }
        m_mprisService->setBlacklist(m_configService.config().shell.mpris.blacklist);
      };
      applyMprisConfig();
      m_configService.addReloadCallback(applyMprisConfig);
      m_mprisService->setChangeCallback([this, shouldRefreshControlCenter]() {
        m_bar.refresh();
        m_mediaOsd.onMprisChanged(*m_mprisService);
        if (shouldRefreshControlCenter()) {
          m_panelManager.refresh();
        }
      });
      kLog.info("mpris discovery active");
    } catch (const std::exception& e) {
      kLog.warn("mpris disabled: {}", e.what());
      m_mprisService.reset();
      m_notificationManager.addInternal(
          "Noctalia", i18n::tr("notifications.internal.mpris-disabled"), e.what(), Urgency::Low
      );
    }

    installNotificationBusNameWatch();
    syncNotificationDaemon();
    m_configService.addReloadCallback([this]() { syncNotificationDaemon(); });

    m_compositorPlatform.startKdeActiveWindow(*m_bus);

    m_trayService = std::make_unique<TrayService>(*m_bus);
    m_trayService->setChangeCallback([this]() {
      m_bar.refresh();
      m_trayMenu.onTrayChanged();
    });
    m_trayService->setMenuToggleCallback([this](const std::string& itemId, float contentScale) {
      m_trayMenu.toggleForItem(itemId, contentScale);
    });
  }

  m_locationService.initialize();
  m_weatherService.initialize();
  m_calendarService.initialize();

  // LocationService is the single source of "where am I": push its resolved coordinates to the
  // weather service, night light, and theme auto mode. Manual latitude/longitude and fixed
  // sunrise/sunset live in [location] and reach night light/theme through their config reloads.
  auto pushLocation = [this]() {
    m_gammaService.setLocationResolving(m_locationService.resolving());
    const auto location = m_locationService.resolvedLocation();
    if (location.has_value()) {
      m_weatherService.setLocation(
          WeatherCoordinates{.latitude = location->latitude, .longitude = location->longitude}, location->name,
          location->sourceLabel
      );
      m_gammaService.setResolvedCoordinates(location->latitude, location->longitude);
      m_themeService.setAutoCoordinates(location->latitude, location->longitude);
    } else {
      m_weatherService.setLocation(std::nullopt, {}, {});
      m_gammaService.setResolvedCoordinates(std::nullopt, std::nullopt);
      m_themeService.setAutoCoordinates(std::nullopt, std::nullopt);
    }
  };
  pushLocation();
  m_locationService.addChangeCallback([this, pushLocation, shouldRefreshControlCenter]() {
    pushLocation();
    m_bar.refresh();
    m_desktopWidgetsController.requestLayout();
    if (shouldRefreshControlCenter()) {
      m_panelManager.refresh();
    }
  });
  m_weatherService.addChangeCallback([this, shouldRefreshControlCenter]() {
    m_bar.refresh();
    m_desktopWidgetsController.requestLayout();
    if (shouldRefreshControlCenter()) {
      m_panelManager.refresh();
    }
  });
}

void Application::startTrayService() {
  if (m_bus == nullptr || m_trayService == nullptr) {
    return;
  }

  try {
    m_trayService->start();
  } catch (const std::exception& e) {
    kLog.warn("tray watcher disabled: {}", e.what());
  }
}

void Application::initUi() {
  auto shouldRefreshControlCenter = [this]() { return m_panelManager.isOpenPanel("control-center"); };

  m_renderContext.initialize(m_glShared);
  m_renderContext.setGraphicsResetCallback([this](RenderGraphicsResetStatus status) { onGraphicsReset(status); });
  if (!m_glShared.hasSharedContext()) {
    m_asyncTextureCache.setMakeCurrentCallback([this]() { m_renderContext.backend().makeCurrentNoSurface(); });
  }
  m_renderContext.setTextFontFamily(m_configService.config().shell.fontFamily);
  m_wallpaper.initialize(m_wayland, &m_configService, &m_renderContext, &m_sharedTextureCache);
  m_backdrop.initialize(m_wayland, &m_configService, &m_sharedTextureCache, &m_glShared);
  m_settingsWindow.initialize(
      m_wayland, &m_configService, &m_renderContext, &m_dependencyService, m_upowerService.get(), &m_idleManager,
      &m_compositorPlatform, m_accountsService.get()
  );
  m_settingsWindow.setPluginManager(&m_pluginManager);
  m_settingsWindow.setOpenDesktopWidgetEditor([this]() {
    if (m_lockscreenWidgetsController.isEditing()) {
      m_lockscreenWidgetsController.exitEdit();
    }
    const bool wasEditing = m_desktopWidgetsController.isEditing();
    m_desktopWidgetsController.toggleEdit();
    if (!wasEditing && m_desktopWidgetsController.isEditing()) {
      notify::info(
          "Noctalia", i18n::tr("notifications.internal.desktop-widgets-editor"),
          i18n::tr("notifications.internal.desktop-widgets-editor-enabled")
      );
    }
  });
  m_settingsWindow.setOpenLockscreenWidgetEditor([this]() {
    if (!m_configService.isLockScreenEnabled()) {
      return;
    }
    if (m_lockScreen.isActive()) {
      notify::info(
          "Noctalia", i18n::tr("notifications.internal.lockscreen-widgets-editor"),
          i18n::tr("notifications.internal.lockscreen-widgets-editor-blocked-locked")
      );
      return;
    }
    if (m_desktopWidgetsController.isEditing()) {
      m_desktopWidgetsController.exitEdit();
    }
    const bool wasEditing = m_lockscreenWidgetsController.isEditing();
    m_lockscreenWidgetsController.toggleEdit();
    if (!wasEditing && m_lockscreenWidgetsController.isEditing()) {
      if (m_settingsWindow.isOpen()) {
        m_settingsWindow.close();
      }
      notify::info(
          "Noctalia", i18n::tr("notifications.internal.lockscreen-widgets-editor"),
          i18n::tr("notifications.internal.lockscreen-widgets-editor-enabled")
      );
    }
  });
  m_settingsWindow.setSyncGreeterAppearance([this]() {
    (void)greeter::syncAppearanceToGreeterAsync(
        m_configService, m_themeService.resolvedMode(),
        [this](bool success) {
          DeferredCall::callLater([this, success]() {
            if (success) {
              notify::info(
                  "Noctalia", i18n::tr("notifications.internal.greeter-sync"),
                  i18n::tr("notifications.internal.greeter-sync-success")
              );
              return;
            }
            m_settingsWindow.markSettingsWriteError(i18n::tr("settings.errors.sync-greeter"));
          });
        },
        &m_compositorPlatform
    );
  });
  m_settingsWindow.setSaveWallpaperPaletteAsCustom([this]() {
    std::string paletteName;
    std::string error;
    if (!m_themeService.saveWallpaperPaletteAsCustom(&paletteName, &error)) {
      m_settingsWindow.markSettingsWriteError(
          error.empty() ? i18n::tr("settings.errors.export-wallpaper-palette") : std::move(error)
      );
      return;
    }
    m_settingsWindow.onExternalOptionsChanged();
    m_settingsWindow.markSettingsWriteSuccess(true);
    notify::info(
        "Noctalia", i18n::tr("notifications.internal.wallpaper-palette-export"),
        i18n::tr("notifications.internal.wallpaper-palette-export-success", "name", paletteName)
    );
  });
  m_lockScreen.initialize(m_wayland, &m_renderContext, &m_configService, &m_sharedTextureCache, m_systemBus.get());
  m_wallpaper.setAutomationGate([this]() { return !m_lockScreen.isActive(); });
  m_configService.addReloadCallback([this]() {
    if (m_logindService != nullptr) {
      m_logindService->setSessionLockIntegrationEnabled(m_configService.isLockScreenEnabled());
    }
    m_lockScreen.onConfigChanged();
    m_lockscreenWidgetsController.onLockStateChanged();
  });
  m_lockScreen.setSessionHooks(
      [this]() {
        m_lockscreenWidgetsController.onLockStateChanged();
        m_hookManager.fire(HookKind::SessionLocked);
      },
      [this]() {
        m_lockscreenWidgetsController.onLockStateChanged();
        m_hookManager.fire(HookKind::SessionUnlocked);
        if (m_logindService != nullptr) {
          m_logindService->syncSessionUnlocked();
        }
      }
  );
  if (m_logindService != nullptr) {
    m_logindService->setSessionLockIntegrationEnabled(m_configService.isLockScreenEnabled());
    m_logindService->setLockCallback([this]() {
      if (!m_configService.isLockScreenEnabled()) {
        return;
      }
      if (!m_lockScreen.isActive()) {
        (void)m_lockScreen.lock();
      }
    });
    m_logindService->setUnlockCallback([this]() {
      if (m_lockScreen.isActive()) {
        m_lockScreen.unlock();
      }
    });
    m_lockScreen.setLockEngagedCallback([this]() {
      if (!m_configService.isLockScreenEnabled() || m_logindService == nullptr) {
        return;
      }
      m_logindService->syncSessionLocked();
    });
  }

  SessionActionHooks sessionActionHooks;
  sessionActionHooks.onLogout = [this]() { return m_hookManager.fireBlocking(HookKind::LoggingOut); };
  sessionActionHooks.onReboot = [this]() { return m_hookManager.fireBlocking(HookKind::Rebooting); };
  sessionActionHooks.onShutdown = [this]() { return m_hookManager.fireBlocking(HookKind::ShuttingDown); };
  m_sessionActionRunner.setHooks(std::move(sessionActionHooks));
  m_sessionActionRunner.setPowerConfig(m_configService.config().shell.session.power);
  m_configService.addReloadCallback(
      [this]() { m_sessionActionRunner.setPowerConfig(m_configService.config().shell.session.power); }, "session-power"
  );

  m_wayland.setPointerEventCallback([this](const PointerEvent& event) {
    if (m_lockScreen.isActive()) {
      m_lockScreen.onPointerEvent(event);
      return;
    }
    if (m_colorPickerDialogPopup.onPointerEvent(event)) {
      return;
    }
    if (m_glyphPickerDialogPopup.onPointerEvent(event)) {
      return;
    }
    if (m_fileDialogPopup.onPointerEvent(event)) {
      return;
    }
    if (m_lockscreenWidgetsController.onPointerEvent(event)) {
      return;
    }
    if (m_desktopWidgetsController.onPointerEvent(event)) {
      return;
    }
    if (m_wallpaper.onPointerEvent(event)) {
      return;
    }
    if (m_screenshotService.onPointerEvent(event)) {
      return;
    }
    if (m_trayMenu.onPointerEvent(event)) {
      return;
    }
    if (m_settingsWindow.onPointerEvent(event))
      return;
    if (m_bar.onPointerEvent(event))
      return;
    if (m_dock.onPointerEvent(event))
      return;
    if (m_windowSwitcher.onPointerEvent(event))
      return;
    if (m_panelManager.onPointerEvent(event))
      return;
    m_notificationToast.onPointerEvent(event);
  });

  m_wayland.setKeyboardEventCallback([this](const KeyboardEvent& event) {
    if (m_lockScreen.isActive()) {
      m_lockScreen.onKeyboardEvent(event);
      return;
    }
    if (m_colorPickerDialogPopup.isOpen()) {
      m_colorPickerDialogPopup.onKeyboardEvent(event);
      return;
    }
    if (m_glyphPickerDialogPopup.isOpen()) {
      m_glyphPickerDialogPopup.onKeyboardEvent(event);
      return;
    }
    if (m_fileDialogPopup.isOpen()) {
      m_fileDialogPopup.onKeyboardEvent(event);
      return;
    }
    if (m_lockscreenWidgetsController.isEditing()) {
      m_lockscreenWidgetsController.onKeyboardEvent(event);
      return;
    }
    if (m_desktopWidgetsController.isEditing()) {
      m_desktopWidgetsController.onKeyboardEvent(event);
      return;
    }
    if (m_settingsWindow.ownsKeyboardSurface(m_wayland.lastKeyboardSurface())) {
      m_settingsWindow.onKeyboardEvent(event);
      return;
    }
    if (m_screenshotService.onKeyboardEvent(event)) {
      return;
    }
    if (m_overviewLauncherCapture.handleKeyboardEvent(event)) {
      return;
    }
    if (m_notificationToast.onKeyboardEvent(event)) {
      return;
    }
    if (m_windowSwitcher.onKeyboardEvent(event)) {
      return;
    }
    m_panelManager.onKeyboardEvent(event);
  });

  // Panel manager must be before bar so widgets can access PanelManager::instance()
  m_panelManager.initialize(m_compositorPlatform, &m_configService, &m_renderContext);
  m_panelManager.setOpenSettingsWindowCallback([this]() { m_settingsWindow.open(); });
  m_panelManager.setCloseSettingsWindowCallback([this]() { m_settingsWindow.close(); });
  m_panelManager.setToggleSettingsWindowCallback([this]() {
    if (m_settingsWindow.isOpen()) {
      m_settingsWindow.close();
      return;
    }
    m_settingsWindow.open();
  });
  m_panelManager.setCloseDesktopWidgetsEditorCallback([this]() {
    if (m_desktopWidgetsController.isEditing()) {
      m_desktopWidgetsController.exitEdit();
    }
  });
  m_settingsWindow.setOpenWallpaperPanel([this]() {
    wl_output* output = m_compositorPlatform.preferredInteractiveOutput(std::chrono::milliseconds(1200));
    m_panelManager.openPanel("wallpaper", PanelOpenRequest{.output = output});
  });
  m_settingsWindow.setConnectCalendarAccount([this](std::string accountId, std::string activationToken) {
    const auto& accounts = m_configService.config().calendar.accounts;
    const auto it = std::ranges::find(accounts, accountId, &CalendarConfig::Account::id);
    if (it == accounts.end()) {
      return;
    }
    if (it->type == "google") {
      m_calendarService.connectGoogleAccount(accountId, activationToken);
    } else if (it->type == "caldav") {
      m_calendarService.requestRefresh();
    }
  });
  auto clipboardPanel = std::make_unique<ClipboardPanel>(
      &m_clipboardService, &m_configService, &m_thumbnailService, &m_asyncTextureCache
  );
  clipboardPanel->setActivateCallback([this](const ClipboardEntry& entry) {
    m_panelManager.close();
    const ClipboardAutoPasteMode mode = m_configService.config().shell.clipboardAutoPaste;
    if (mode == ClipboardAutoPasteMode::Off) {
      return;
    }
    const bool isImage = entry.isImage();
    m_clipboardAutoPasteTimer.stop();
    m_clipboardAutoPasteTimer.start(std::chrono::milliseconds(Style::animFast + 30), [this, isImage]() {
      DeferredCall::callLater([this, isImage]() {
        const ClipboardAutoPasteMode activeMode = m_configService.config().shell.clipboardAutoPaste;
        (void)clipboard_paste::pasteEntry(isImage, activeMode, m_virtualKeyboardService);
      });
    });
  });
  m_panelManager.registerPanel("clipboard", std::move(clipboardPanel));
  syncClipboardService();
  m_panelManager.registerPanel("session", std::make_unique<SessionPanel>(&m_configService, m_sessionActionRunner));
  m_panelManager.registerPanel("test", std::make_unique<TestPanel>());
  m_panelManager.registerPanel(
      "control-center",
      std::make_unique<ControlCenterPanel>(ControlCenterServices{
          .notifications = &m_notificationManager,
          .audio = m_pipewireService.get(),
          .easyEffects = m_easyEffectsService.get(),
          .mpris = m_mprisService.get(),
          .config = &m_configService,
          .httpClient = &m_httpClient,
          .weather = &m_weatherService,
          .spectrum = m_pipewireSpectrum.get(),
          .upower = m_upowerService.get(),
          .powerProfiles = m_powerProfilesService.get(),
          .network = m_networkService.get(),
          .networkSecrets = m_networkSecretAgent.get(),
          .bluetooth = m_bluetoothService.get(),
          .bluetoothAgent = m_bluetoothAgent.get(),
          .brightness = m_brightnessService.get(),
          .sysmon = m_systemMonitor.get(),
          .screenTime = &m_screenTimeService,
          .nightLight = &m_gammaService,
          .theme = &m_themeService,
          .idleInhibitor = &m_idleInhibitor,
          .dependencies = &m_dependencyService,
          .platform = &m_compositorPlatform,
          .ipc = &m_ipcService,
          .wallpaper = &m_wallpaper,
          .calendar = &m_calendarService,
          .scriptApi = &m_scriptApi,
          .fileWatcher = &m_fileWatcher,
          .clipboard = &m_clipboardService,
          .accounts = m_accountsService.get(),
          .thumbnails = &m_thumbnailService,
      })
  );
  {
    auto launcherPanel = std::make_unique<LauncherPanel>(&m_configService, &m_asyncTextureCache);
    launcherPanel->addProvider(std::make_unique<AppProvider>(&m_configService, &m_compositorPlatform));
    launcherPanel->addProvider(std::make_unique<WallpaperProvider>(&m_configService, &m_wayland));
    launcherPanel->addProvider(std::make_unique<WindowProvider>(&m_compositorPlatform));
    launcherPanel->addProvider(std::make_unique<SessionProvider>(&m_configService, &m_sessionActionRunner));
    launcherPanel->addProvider(std::make_unique<MathProvider>(&m_clipboardService, &m_configService, &m_httpClient));
    launcherPanel->addProvider(std::make_unique<EmojiProvider>(&m_clipboardService));
    m_launcherPanel = launcherPanel.get();
    m_panelManager.registerPanel("launcher", std::move(launcherPanel));
  }
  m_settingsWindow.setResetLauncherUsage([this]() {
    if (m_launcherPanel != nullptr) {
      m_launcherPanel->clearUsage();
    }
    notify::info(
        "Noctalia", i18n::tr("notifications.internal.launcher-usage-reset"),
        i18n::tr("notifications.internal.launcher-usage-reset-success")
    );
  });
  m_settingsWindow.setResetScreenTime([this]() {
    m_screenTimeService.clearAll();
    notify::info(
        "Noctalia", i18n::tr("notifications.internal.screen-time-reset"),
        i18n::tr("notifications.internal.screen-time-reset-success")
    );
  });
  reloadPluginLauncherProviders();
  m_overviewLauncherCapture.initialize(m_wayland, &m_renderContext, m_compositorPlatform, m_panelManager);
  m_overviewLauncherCapture.setEnabled(m_configService.config().shell.niriOverviewTypeToLaunchEnabled);
  m_overviewLauncherCapture.setOpenLauncherCallback(
      [this](std::string_view initialQuery, wl_output* output, std::string_view sourceBarName) {
        if (m_panelManager.isOpenPanel("launcher")) {
          return;
        }
        m_panelManager.openPanel(
            "launcher", PanelOpenRequest{.output = output, .context = initialQuery, .sourceBarName = sourceBarName}
        );
      }
  );
  m_compositorPlatform.setOverviewChangeCallback([this]() { m_overviewLauncherCapture.sync(); });
  m_panelManager.setPanelOpenedCallback([this]() {
    m_overviewLauncherCapture.sync();
    if (m_panelManager.isAttachedOpen()) {
      m_bar.revealAutoHideForAttachedPanel(
          m_panelManager.attachedPanelOutput(), m_panelManager.attachedSourceBarName()
      );
    }
  });
  m_panelManager.setPanelClosedCallback([this]() {
    m_overviewLauncherCapture.sync();
    m_bar.reevaluateAutoHide();
  });
  m_configService.addReloadCallback([this]() {
    m_overviewLauncherCapture.setEnabled(m_configService.config().shell.niriOverviewTypeToLaunchEnabled);
  });
  m_overviewLauncherCapture.sync();
  m_panelManager.registerPanel(
      "wallpaper",
      std::make_unique<WallpaperPanel>(&m_wayland, &m_configService, &m_thumbnailService, &m_wallpaperScanner)
  );
  std::size_t trayDrawerColumns = 3;
  if (const auto it = m_configService.config().widgets.find("tray"); it != m_configService.config().widgets.end()) {
    trayDrawerColumns =
        static_cast<std::size_t>(std::clamp<std::int64_t>(it->second.getInt("drawer_columns", 3), 1, 5));
  }
  m_panelManager.registerPanel(
      "tray-drawer", std::make_unique<TrayDrawerPanel>(m_trayService.get(), &m_configService, trayDrawerColumns)
  );
  m_panelManager.registerPanel("polkit", std::make_unique<PolkitPanel>(&m_configService, [this]() {
                                 return m_polkitAgent.get();
                               }));
  m_panelManager.registerPanel("setup-wizard", std::make_unique<SetupWizardPanel>(&m_configService, &m_wayland));

  if (SetupWizardPanel::isFirstRun(m_configService)) {
    DeferredCall::callLater([]() { PanelManager::instance().togglePanel("setup-wizard"); });
  }

  m_notificationToast.initialize(m_wayland, &m_configService, &m_notificationManager, &m_renderContext, &m_httpClient);
  m_configService.addReloadCallback([this]() { m_notificationToast.onConfigReload(); });
  auto applyNotificationFilterConfig = [this]() {
    m_notificationManager.setFilters(m_configService.config().notification.filters);
  };
  applyNotificationFilterConfig();
  m_configService.addReloadCallback(applyNotificationFilterConfig);
  m_configService.setNotificationManager(&m_notificationManager);
  m_notificationManager.setSoundPlayer(m_soundPlayer.get());

  TooltipManager::instance().initialize(m_wayland, &m_renderContext);
  m_osdOverlay.initialize(m_wayland, &m_configService, &m_renderContext);
  m_windowSwitcher.initialize(
      m_wayland, &m_renderContext, m_compositorPlatform, &m_configService, &m_asyncTextureCache
  );
  m_configService.addReloadCallback([this]() { m_osdOverlay.onConfigReload(); });
  m_idleGraceOverlay.initialize(m_wayland, &m_renderContext);
  m_wayland.setIdleCapabilitiesReadyCallback([this]() { m_idleManager.reload(m_configService.config().idle); });
  m_idleManager.initialize(
      m_wayland,
      [this](
          const std::string& behaviorName, std::chrono::milliseconds fadeIn, bool willLockSession,
          std::function<void()> onFadeComplete
      ) {
        (void)behaviorName;
        // Snapshot the clean desktop before the overlay fades in
        if (willLockSession && m_configService.isLockScreenEnabled()) {
          m_lockScreen.primeDesktopCaptures();
        }
        DeferredCall::callLater([this, fadeIn, done = std::move(onFadeComplete)]() mutable {
          m_idleGraceOverlay.show(fadeIn, std::move(done));
        });
      },
      [this](bool userCancelled) {
        DeferredCall::callLater([this, userCancelled]() {
          m_idleGraceOverlay.hide();
          if (userCancelled) {
            m_lockScreen.clearPrimedDesktopCaptures();
          }
        });
      }
  );
  m_idleManager.setActionRunner(
      [this](const IdleBehaviorConfig& /*behavior*/, const IdleActionRequest& action) -> bool {
        return runIdleAction(action);
      }
  );
  m_idleManager.setLiveIdleChangeCallback([this]() {
    DeferredCall::callLater([this]() { m_settingsWindow.onIdleLiveStatusChanged(); });
  });
  m_idleManager.reload(m_configService.config().idle);
  try {
    m_screenSaverService = std::make_unique<ScreenSaverService>(m_systemBus.get());
    if (m_screenSaverService->active()) {
      m_screenSaverService->setChangeCallback([this](std::int64_t locks) {
        m_idleManager.setScreenSaverInhibitLocks(locks);
      });
      m_idleManager.setScreenSaverInhibitLocks(m_screenSaverService->inhibitLocks());
    } else {
      m_screenSaverService.reset();
    }
  } catch (const std::exception& e) {
    kLog.warn("idle inhibit service disabled: {}", e.what());
    m_screenSaverService.reset();
  }
  m_configService.addReloadCallback(
      [this]() {
        if (m_configService.lastChange().idle) {
          m_idleManager.reload(m_configService.config().idle);
        }
      },
      "idle"
  );
  m_audioOsd.bindOverlay(m_osdOverlay);
  m_audioOsd.setSoundPlayer(m_soundPlayer.get());
  if (m_pipewireService != nullptr) {
    m_audioOsd.primeFromService(*m_pipewireService);
  }
  m_brightnessOsd.bindOverlay(m_osdOverlay);
  if (m_brightnessService != nullptr) {
    m_brightnessOsd.primeFromService(*m_brightnessService);
  }
  if constexpr (kLockKeysEnabled) {
    m_lockKeysOsd.bindOverlay(m_osdOverlay);
    m_lockKeysOsd.primeFromService(m_lockKeysService);
  }
  m_keyboardLayoutOsd.bindOverlay(m_osdOverlay);
  m_keyboardLayoutOsd.prime(m_compositorPlatform);
  m_mediaOsd.bindOverlay(m_osdOverlay);
  m_privacyOsd.bindOverlay(m_osdOverlay);
  m_privacyOsd.configure(m_configService.config());
  m_configService.addReloadCallback(
      [this]() {
        if (m_configService.lastChange().shell) {
          m_privacyOsd.onConfigReload(m_configService.config(), m_pipewireService.get());
          m_bar.refresh();
        }
      },
      "privacy-filters"
  );
  m_screenCorners.initialize(m_wayland, &m_configService, &m_renderContext);
  m_screenCorners.onConfigReload();

  m_trayMenu.initialize(m_wayland, &m_configService, m_trayService.get(), &m_renderContext);

  m_bar.initialize({
      .platform = m_compositorPlatform,
      .config = m_configService,
      .notifications = &m_notificationManager,
      .tray = m_trayService.get(),
      .audio = m_pipewireService.get(),
      .easyEffects = m_easyEffectsService.get(),
      .upower = m_upowerService.get(),
      .sysmon = m_systemMonitor.get(),
      .powerProfiles = m_powerProfilesService.get(),
      .network = m_networkService.get(),
      .idleInhibitor = &m_idleInhibitor,
      .mpris = m_mprisService.get(),
      .audioSpectrum = m_pipewireSpectrum.get(),
      .httpClient = &m_httpClient,
      .weather = &m_weatherService,
      .renderContext = &m_renderContext,
      .nightLight = &m_gammaService,
      .theme = &m_themeService,
      .bluetooth = m_bluetoothService.get(),
      .brightness = m_brightnessService.get(),
      .lockKeys = kLockKeysEnabled ? &m_lockKeysService : nullptr,
      .clipboard = &m_clipboardService,
      .fileWatcher = &m_fileWatcher,
      .screenshots = &m_screenshotService,
      .scriptApi = &m_scriptApi,
  });
  m_idleInhibitor.setAnchorSurfacesProvider([this]() { return m_bar.caffeineAnchorSurfaces(); });
  m_bar.setOpenWidgetSettingsCallback([this](std::string barName, std::string widgetName) {
    if (m_panelManager.isOpen()) {
      m_panelManager.closePanel();
    }
    m_settingsWindow.openToBarWidget(std::move(barName), std::move(widgetName));
  });
  m_panelManager.setAttachedPanelGeometryCallback(
      [this](wl_output* output, std::string_view barName, std::optional<AttachedPanelGeometry> geometry) {
        m_bar.setAttachedPanelGeometry(output, barName, geometry);
      }
  );
  m_panelManager.setClickShieldExcludeRectsProvider([this](wl_output* output) {
    return m_bar.surfaceRectsForOutput(output);
  });
  m_panelManager.setFocusGrabBarSurfacesProvider([this]() { return m_bar.allBarSurfaces(); });
  m_panelManager.setAttachedPanelAvailabilityCallback([this](wl_output* output, std::string_view barName) {
    return m_bar.canAttachPanelToBar(output, barName);
  });
  m_panelManager.setAttachedPanelBarSettledCallback([this](wl_output* output, std::string_view barName) {
    return m_bar.isAttachedPanelBarSettled(output, barName);
  });
  m_bar.setAutoHideSuppressionCallback([this](const BarInstance& instance) {
    if (m_trayMenu.isOpen()) {
      return true;
    }
    return m_panelManager.isAttachedOpen() && m_panelManager.attachedSourceBarName() == instance.barConfig.name;
  });
  // When config reloads, refresh any open panel: bar-driven attached decoration restyle and
  // shell-driven compositor blur.
  m_configService.addReloadCallback([this]() { m_panelManager.onConfigReloaded(); });
  m_configService.addReloadCallback([this]() { m_screenCorners.onConfigReload(); });

  m_layerPopupHosts.registerHost(
      [this](wl_surface* surface) {
        if (auto context = m_lockscreenWidgetsController.popupParentContextForSurface(surface); context.has_value()) {
          return context;
        }
        return m_desktopWidgetsController.popupParentContextForSurface(surface);
      },
      {}, {},
      [this]() {
        if (auto context = m_lockscreenWidgetsController.fallbackPopupParentContext(); context.has_value()) {
          return context;
        }
        return m_desktopWidgetsController.fallbackPopupParentContext();
      }
  );
  m_layerPopupHosts.registerHost(
      [this](wl_surface* surface) { return m_panelManager.popupParentContextForSurface(surface); },
      [this](wl_surface* surface) { m_panelManager.beginAttachedPopup(surface); },
      [this](wl_surface* surface) { m_panelManager.endAttachedPopup(surface); },
      [this]() { return m_panelManager.fallbackPopupParentContext(); }
  );
  m_layerPopupHosts.registerHost(
      [this](wl_surface* surface) { return m_bar.popupParentContextForSurface(surface); },
      [this](wl_surface* surface) { m_bar.beginAttachedPopup(surface); },
      [this](wl_surface* surface) { m_bar.endAttachedPopup(surface); },
      [this]() {
        return m_bar.preferredPopupParentContext(
            m_compositorPlatform.preferredInteractiveOutput(std::chrono::milliseconds(1200))
        );
      }
  );
  m_layerPopupHosts.registerHost(
      [this](wl_surface* surface) { return m_settingsWindow.popupParentContextForSurface(surface); }, {}, {},
      [this]() { return m_settingsWindow.fallbackPopupParentContext(); }
  );

  m_colorPickerDialogPopup.initialize(m_wayland, m_configService, m_renderContext, m_layerPopupHosts);
  ColorPickerDialog::setPresenter(&m_colorPickerDialogPopup);

  m_glyphPickerDialogPopup.initialize(m_wayland, m_configService, m_renderContext, m_layerPopupHosts);
  GlyphPickerDialog::setPresenter(&m_glyphPickerDialogPopup);

  m_fileDialogPopup.initialize(m_wayland, m_configService, m_renderContext, m_layerPopupHosts, m_thumbnailService);
  FileDialog::setPresenter(&m_fileDialogPopup);

  m_dock.initialize(m_compositorPlatform, &m_configService, &m_renderContext);
  const DesktopWidgetScriptDeps desktopWidgetScriptDeps{
      .scriptApi = &m_scriptApi,
      .fileWatcher = &m_fileWatcher,
      .clipboard = &m_clipboardService,
      .configService = &m_configService,
  };
  const DesktopWidgetRuntimeServices desktopWidgetRuntime{
      .pipewireSpectrum = m_pipewireSpectrum.get(),
      .weather = &m_weatherService,
      .mpris = m_mprisService.get(),
      .httpClient = &m_httpClient,
      .sysmon = m_systemMonitor.get(),
      .scriptDeps = desktopWidgetScriptDeps,
  };
  const DesktopWidgetServices lockscreenWidgetServices{
      .wayland = m_wayland,
      .config = &m_configService,
      .renderContext = &m_renderContext,
      .runtime = desktopWidgetRuntime,
      .textureCache = &m_sharedTextureCache,
  };
  const DesktopWidgetServices desktopWidgetServices{
      .wayland = m_wayland,
      .config = &m_configService,
      .renderContext = &m_renderContext,
      .runtime = desktopWidgetRuntime,
  };
  m_lockscreenWidgetsController.initialize({
      .widgets = lockscreenWidgetServices,
      .lockScreen = m_lockScreen,
      .bar = m_bar,
      .dock = m_dock,
      .desktopWidgets = &m_desktopWidgetsController,
  });
  m_desktopWidgetsController.initialize({
      .widgets = desktopWidgetServices,
      .lockscreenWidgets = &m_lockscreenWidgetsController,
  });
  m_desktopWidgetsController.setOnEnterEditCallback([this]() {
    if (m_settingsWindow.isOpen()) {
      m_settingsWindow.close();
    }
  });
  m_iconThemePollSource.setChangeCallback([this]() { onIconThemeChanged(); });

  std::string lastShellFontFamily = m_configService.config().shell.fontFamily;
  m_configService.addReloadCallback(
      [this, lastShellFontFamily]() mutable {
        const std::string& newShellFontFamily = m_configService.config().shell.fontFamily;
        if (newShellFontFamily == lastShellFontFamily) {
          return;
        }

        lastShellFontFamily = newShellFontFamily;
        text::invalidateFontWeightCatalogCache();
        m_renderContext.setTextFontFamily(newShellFontFamily);
        m_bar.requestLayout();
        m_dock.requestLayout();
        m_desktopWidgetsController.requestLayout();
        m_lockscreenWidgetsController.requestLayout();
        m_panelManager.requestLayout();
        m_notificationToast.requestLayout();
        m_lockScreen.onFontChanged();
        m_osdOverlay.requestLayout();
        m_trayMenu.onFontChanged();
        m_backdrop.onFontChanged();
        m_settingsWindow.onFontChanged();
        m_colorPickerDialogPopup.requestLayout();
        m_glyphPickerDialogPopup.requestLayout();
        m_fileDialogPopup.requestLayout();
      },
      "shell-font-family"
  );

  m_timeService.setTickSecondCallback([this]() {
    m_wallpaper.onSecondTick();
    if (m_lockScreen.isActive()) {
      m_lockscreenWidgetsController.onSecondTick();
    } else {
      m_bar.onSecondTick();
      m_desktopWidgetsController.onSecondTick();
      m_lockscreenWidgetsController.onSecondTick();
      m_settingsWindow.onSecondTick();
      if (m_configService.config().osd.kinds.keyboardLayout) {
        m_keyboardLayoutOsd.onLayoutChanged(m_compositorPlatform, m_configService.config());
      }
    }
    m_idleManager.onSecondTick();
  });

  if (m_pipewireService != nullptr) {
    m_audioOsd.suppressFor(std::chrono::milliseconds(2000));
    m_pipewireService->setChangeCallback([this, shouldRefreshControlCenter]() {
      if (m_pipewireSpectrum != nullptr) {
        m_pipewireSpectrum->handleAudioStateChanged();
      }
      m_bar.refresh();
      if (shouldRefreshControlCenter()) {
        m_panelManager.refresh();
      }
      if (m_pipewireService != nullptr) {
        m_audioOsd.onAudioStateChanged(*m_pipewireService);
        m_privacyOsd.onPrivacyStateChanged(*m_pipewireService);
      }
    });
    m_pipewireService->setVolumePreviewCallback([this](bool isInput, std::uint32_t id, float volume, bool muted) {
      if (isInput) {
        m_audioOsd.showInput(id, volume, muted);
      } else {
        m_audioOsd.showOutput(id, volume, muted);
      }
    });
  }
  if (m_easyEffectsService != nullptr) {
    m_easyEffectsService->setChangeCallback([this, shouldRefreshControlCenter]() {
      m_bar.refresh();
      if (shouldRefreshControlCenter()) {
        m_panelManager.refresh();
      }
    });
  }
}

void Application::reloadPluginLauncherProviders() {
  if (m_launcherPanel == nullptr) {
    return;
  }
  m_launcherPanel->clearDynamicProviders();

  auto& registry = scripting::PluginRegistry::instance();
  const auto& pluginSettings = m_configService.config().plugins.pluginSettings;
  static const std::unordered_map<std::string, WidgetSettingValue> kNoOverrides;

  for (const auto& resolved : registry.entriesOfKind(scripting::PluginEntryKind::LauncherProvider)) {
    if (resolved.entry == nullptr || resolved.manifest == nullptr) {
      continue;
    }
    // Launcher providers have no per-instance config, only plugin-level settings.
    auto seeded = scripting::seedEntrySettings(*resolved.entry, kNoOverrides);
    const auto psIt = pluginSettings.find(resolved.manifest->id);
    scripting::mergePluginSettings(
        *resolved.manifest, psIt != pluginSettings.end() ? psIt->second : kNoOverrides, seeded
    );

    std::vector<LauncherCategory> categories;
    categories.reserve(resolved.entry->launcherCategories.size());
    for (const auto& cat : resolved.entry->launcherCategories) {
      categories.push_back(LauncherCategory{.label = cat.label, .glyphName = cat.glyph});
    }

    m_launcherPanel->addProvider(
        std::make_unique<PluginLauncherProvider>(
            scripting::PluginRuntimeContext{
                .entryId = resolved.fullId(),
                .sourcePath = resolved.sourcePath,
                .settings = std::move(seeded),
                .scriptApi = m_scriptApi,
                .fileWatcher = &m_fileWatcher,
                .httpClient = &m_httpClient,
                .clipboard = &m_clipboardService,
            },
            PluginLauncherProviderOptions{
                .displayName = resolved.manifest->name,
                .prefix = resolved.entry->launcherPrefix,
                .glyph = resolved.entry->launcherGlyph,
                .globalSearch = resolved.entry->launcherGlobalSearch,
                .debounceMs = resolved.entry->launcherDebounceMs,
                .categories = std::move(categories),
            }
        )
    );
  }
}

void Application::initIpc() {
  if (m_ipcService.start()) {
    kLog.info("IPC socket at {}", m_ipcService.socketPath());
  } else {
    kLog.warn("IPC disabled: could not bind socket");
  }

  m_ipcService.registerHandler(
      "status",
      [this](const std::string&) -> std::string {
        const bool panelOpen = m_panelManager.isOpen();
        std::string json = "{\n";
        json += "  \"barVisible\": ";
        json += m_bar.isVisible() ? "true" : "false";
        json += ",\n  \"panelOpen\": ";
        json += panelOpen ? "true" : "false";
        json += ",\n  \"activePanelId\": ";
        json += panelOpen ? ("\"" + m_panelManager.activePanelId() + "\"") : "null";
        json += "\n}\n";
        return json;
      },
      "status", "Print current state as JSON"
  );

  auto applyNotificationDnd = [this](bool enabled) {
    m_notificationManager.setDoNotDisturb(enabled);
    m_bar.refresh();
    if (m_panelManager.isOpenPanel("control-center")) {
      m_panelManager.refresh();
    }
  };

  m_ipcService.registerHandler(
      "notification-dnd-set",
      [this, applyNotificationDnd](const std::string& args) -> std::string {
        const auto parts = noctalia::ipc::splitWords(args);
        if (parts.size() != 1) {
          return "error: notification-dnd-set requires <on|off|true|false|1|0>\n";
        }
        const std::string& value = parts[0];
        std::optional<bool> nextState;
        if (value == "on" || value == "true" || value == "1") {
          nextState = true;
        } else if (value == "off" || value == "false" || value == "0") {
          nextState = false;
        } else {
          return "error: invalid value (use on/off, true/false, 1/0)\n";
        }

        const bool currentState = m_notificationManager.doNotDisturb();
        if (currentState == *nextState) {
          return "ok\n";
        }
        applyNotificationDnd(*nextState);
        m_osdOverlay.show(dndOsdContent(*nextState));
        return "ok\n";
      },
      "notification-dnd-set <on|off|true|false|1|0>", "Set notification Do Not Disturb state"
  );

  m_ipcService.registerHandler(
      "notification-dnd-toggle",
      [this, applyNotificationDnd](const std::string&) -> std::string {
        const bool nextState = !m_notificationManager.doNotDisturb();
        applyNotificationDnd(nextState);
        m_osdOverlay.show(dndOsdContent(nextState));
        return "ok\n";
      },
      "notification-dnd-toggle", "Toggle notification Do Not Disturb state"
  );

  m_ipcService.registerHandler(
      "notification-dnd-status",
      [this](const std::string&) -> std::string { return m_notificationManager.doNotDisturb() ? "on\n" : "off\n"; },
      "notification-dnd-status", "Print notification Do Not Disturb state"
  );

  m_ipcService.registerHandler(
      "notification-clear-active",
      [this](const std::string&) -> std::string {
        std::vector<uint32_t> activeIds;
        activeIds.reserve(m_notificationManager.all().size());
        for (const auto& notification : m_notificationManager.all()) {
          activeIds.push_back(notification.id);
        }
        for (const uint32_t id : activeIds) {
          (void)m_notificationManager.close(id, CloseReason::Dismissed);
        }
        if (m_panelManager.isOpenPanel("control-center")) {
          m_panelManager.refresh();
        }
        return "ok\n";
      },
      "notification-clear-active", "Dismiss all currently active notifications"
  );

  m_ipcService.registerHandler(
      "notification-clear-history",
      [this](const std::string&) -> std::string {
        m_notificationManager.clearHistory();
        if (m_panelManager.isOpenPanel("control-center")) {
          m_panelManager.refresh();
        }
        return "ok\n";
      },
      "notification-clear-history", "Clear notification history"
  );

  m_ipcService.registerHandler(
      "clipboard-clear",
      [this](const std::string&) -> std::string {
        m_panelManager.clearClipboardHistory();
        return "ok\n";
      },
      "clipboard-clear", "Clear clipboard history"
  );

  m_ipcService.registerHandler(
      "dpms-on",
      [this](const std::string&) -> std::string {
        if (!m_compositorPlatform.setOutputPower(true)) {
          return "error: failed to execute dpms-on command\n";
        }
        return "ok\n";
      },
      "dpms-on", "Turn monitors on"
  );

  m_ipcService.registerHandler(
      "dpms-off",
      [this](const std::string&) -> std::string {
        if (!m_compositorPlatform.setOutputPower(false)) {
          return "error: failed to execute dpms-off command\n";
        }
        return "ok\n";
      },
      "dpms-off", "Turn monitors off"
  );

  registerSessionIpc(m_ipcService, m_sessionActionRunner, m_lockScreen, m_configService);

  if (m_powerProfilesService != nullptr) {
    m_powerProfilesService->registerIpc(m_ipcService, [this](std::string_view profile) {
      m_osdOverlay.show(powerProfileOsdContent(profile));
    });
  }
  if (m_networkService != nullptr) {
    m_networkService->registerIpc(m_ipcService, [this](bool enabled) { m_osdOverlay.show(wifiOsdContent(enabled)); });
  }
  if (m_bluetoothService != nullptr) {
    m_bluetoothService->registerIpc(m_ipcService, [this](bool enabled) {
      m_osdOverlay.show(bluetoothOsdContent(enabled));
    });
  }

  if (m_brightnessService != nullptr) {
    m_brightnessService->registerIpc(m_ipcService, [this]() {
      m_brightnessOsd.suppressFor(std::chrono::milliseconds(250));
    });
  }
  m_ipcService.registerHandler(
      "brightness-osd",
      [this](const std::string& args) -> std::string {
        const auto parts = noctalia::ipc::splitWords(args);
        if (parts.size() != 1) {
          return "error: brightness-osd requires <value>\n";
        }
        const auto value = noctalia::ipc::parseNormalizedOrPercent(parts[0]);
        if (!value.has_value()) {
          return "error: invalid brightness value (use percent like 65 or 65%, or normalized like 0.65)\n";
        }
        m_brightnessOsd.showValue(*value);
        return "ok\n";
      },
      "brightness-osd <value>", "Show brightness OSD without changing brightness"
  );
  m_configService.registerIpc(m_ipcService);
  scripting::PluginIpcRouter::instance().setPlatform(&m_compositorPlatform);
  m_ipcService.registerHandler(
      "plugin",
      [](const std::string& args) -> std::string { return scripting::PluginIpcRouter::instance().dispatch(args); },
      "plugin <author/plugin:entry> <target[:bar-name]> <event> [payload]", "Dispatch an event to a plugin entry"
  );
  m_ipcService.registerHandler(
      "plugins",
      [this](const std::string& args) -> std::string {
        const auto parts = noctalia::ipc::splitWords(args);
        if (parts.empty()) {
          return "error: plugins <list|enable|disable> [author/plugin]\n";
        }
        const std::string& cmd = parts[0];
        if (cmd == "list") {
          std::string out;
          for (const auto& s : m_pluginManager.list()) {
            const std::string dependencies =
                s.dependencies.empty() ? std::string{} : " requires " + StringUtils::join(s.dependencies, ", ");
            out += std::format(
                "{} [{}] {}{}{}{}{}\n", s.id, s.source, s.version.empty() ? "-" : s.version,
                s.enabled ? " enabled" : "", s.compatible ? "" : " incompatible", s.deprecated ? " deprecated" : "",
                dependencies
            );
          }
          return out.empty() ? "(no plugins)\n" : out;
        }
        if (cmd == "enable") {
          if (parts.size() != 2) {
            return "error: plugins enable <author/plugin>\n";
          }
          const auto res = m_pluginManager.enable(parts[1]);
          return res.ok ? "ok\n" : ("error: " + res.error + "\n");
        }
        if (cmd == "disable") {
          if (parts.size() != 2) {
            return "error: plugins disable <author/plugin>\n";
          }
          m_pluginManager.disable(parts[1]);
          return "ok\n";
        }
        if (cmd == "update") {
          if (parts.size() != 2) {
            return "error: plugins update <source-name>\n";
          }
          m_pluginManager.update(parts[1]);
          return "ok (updating in background)\n";
        }
        if (cmd == "source") {
          if (parts.size() < 2) {
            return "error: plugins source <list|add|remove> ...\n";
          }
          const std::string& sub = parts[1];
          if (sub == "list") {
            std::string out;
            for (const auto& s : m_configService.config().plugins.sources) {
              out += std::format(
                  "{} {} {}{}\n", s.name, enumToKey(kPluginSourceKinds, s.kind), s.location, s.autoUpdate ? " auto" : ""
              );
            }
            return out.empty() ? "(no sources)\n" : out;
          }
          if (sub == "add") {
            if (parts.size() < 5) {
              return "error: plugins source add <name> <git|path> <location> [auto]\n";
            }
            const auto kind = enumFromKey(kPluginSourceKinds, parts[3]);
            if (!kind.has_value()) {
              return "error: source kind must be 'git' or 'path'\n";
            }
            if (!isValidPluginSourceName(parts[2])) {
              return "error: source name must use letters, digits, '.', '_' or '-', starting with a letter or digit\n";
            }
            PluginSourceConfig source{
                .kind = *kind,
                .name = parts[2],
                .location = parts[4],
                .autoUpdate = parts.size() > 5 && (parts[5] == "auto" || parts[5] == "true"),
            };
            m_pluginManager.addSource(source);
            return "ok\n";
          }
          if (sub == "remove") {
            if (parts.size() != 3) {
              return "error: plugins source remove <name>\n";
            }
            if (isDefaultPluginSourceName(parts[2])) {
              return "error: built-in plugin sources cannot be removed from IPC\n";
            }
            m_pluginManager.removeSource(parts[2]);
            return "ok\n";
          }
          return "error: unknown plugins source subcommand '" + sub + "'\n";
        }
        return "error: unknown plugins subcommand '" + cmd + "'\n";
      },
      "plugins <list|enable|disable|update|source> ...",
      "Manage plugins and sources (list/enable/disable/update, source list/add/remove)"
  );
  m_bar.registerIpc(m_ipcService);
  m_desktopWidgetsController.registerIpc(m_ipcService);
  m_lockscreenWidgetsController.registerIpc(m_ipcService);
  m_panelManager.registerIpc(m_ipcService);
  m_idleInhibitor.registerIpc(m_ipcService);
  m_gammaService.registerIpc(m_ipcService);
  m_themeService.registerIpc(m_ipcService);
  m_templateApplyService.registerIpc(m_ipcService);
  m_dock.registerIpc(m_ipcService);
  m_wallpaper.registerIpc(m_ipcService);
  greeter::registerIpc(
      m_ipcService, m_configService, [this]() { return m_themeService.resolvedMode(); }, &m_compositorPlatform
  );
  if (m_mprisService) {
    m_mprisService->registerIpc(m_ipcService);
  }
  if (m_pipewireService) {
    m_pipewireService->registerIpc(m_ipcService, m_configService);
  }
  if (m_easyEffectsService) {
    m_easyEffectsService->registerIpc(
        m_ipcService, m_configService, [this](AudioEffectsProfileKind kind, std::string_view profile) {
          m_osdOverlay.show(effectsProfileOsdContent(kind, profile));
        }
    );
  }
  m_screenshotService.registerIpc(m_ipcService, m_configService);
  m_windowSwitcher.registerIpc(m_ipcService);
}

bool Application::runUserCommand(const std::string& command) {
  constexpr std::string_view prefix = "noctalia:";

  if (command.starts_with(prefix)) {
    const std::string response = m_ipcService.execute(command.substr(prefix.size()));
    if (response.starts_with("error:")) {
      kLog.warn("IPC command '{}' failed: {}", command, response.substr(0, response.find('\n')));
      return false;
    }
    return true;
  }

  if (!process::runAsync(command)) {
    kLog.warn("command failed to launch: {}", command);
    return false;
  }
  return true;
}

bool Application::runUserCommandBlocking(const std::string& command) {
  constexpr std::string_view prefix = "noctalia:";

  if (command.starts_with(prefix)) {
    const std::string response = m_ipcService.execute(command.substr(prefix.size()));
    if (response.starts_with("error:")) {
      kLog.warn("IPC command '{}' failed: {}", command, response.substr(0, response.find('\n')));
      return false;
    }
    return true;
  }

  const auto result = process::runSync(command);
  if (!result) {
    kLog.warn("command failed: {} exit_code={} stderr={}", command, result.exitCode, result.err);
    return false;
  }
  return true;
}

bool Application::runIdleAction(const IdleActionRequest& action) {
  switch (action.kind) {
  case IdleActionKind::None:
    return true;
  case IdleActionKind::Command:
    return runUserCommand(action.command);
  case IdleActionKind::Lock:
    if (!m_configService.isLockScreenEnabled()) {
      return true;
    }
    return m_sessionActionRunner.lock();
  case IdleActionKind::ScreenOff:
    return m_compositorPlatform.setOutputPower(false);
  case IdleActionKind::ScreenOn:
    return m_compositorPlatform.setOutputPower(true);
  case IdleActionKind::Suspend:
    return m_sessionActionRunner.requestSuspendDetached();
  case IdleActionKind::LockAndSuspend:
    if (!m_configService.isLockScreenEnabled()) {
      return m_sessionActionRunner.requestSuspendDetached();
    }
    return m_sessionActionRunner.lockThenSuspendDetached();
  }
  return false;
}

void Application::onIconThemeChanged() {
  kLog.info("system icon theme changed; refreshing icon consumers");
  m_bar.reload();
  m_dock.reload();
  m_panelManager.onIconThemeChanged();
  m_notificationToast.requestLayout();
}

void Application::onGraphicsReset(RenderGraphicsResetStatus status) {
  (void)status;
  m_sharedTextureCache.reloadResidentTextures();
  m_asyncTextureCache.reloadResidentTextures();
  m_thumbnailService.invalidateGpuResources(m_renderContext.backend().textureManager());
  m_wallpaper.onGpuResourcesInvalidated();
  m_backdrop.onGpuResourcesInvalidated();
  m_lockScreen.onGpuResourcesInvalidated();
  m_trayMenu.requestLayout();
  m_settingsWindow.requestRedraw();
  m_screenCorners.requestRedraw();
  requestAllSurfacesRedraw();
}

void Application::requestAllSurfacesRedraw() {
  m_bar.requestRedraw();
  m_dock.requestRedraw();
  m_desktopWidgetsController.requestRedraw();
  m_panelManager.requestRedraw();
  m_notificationToast.requestRedraw();
  m_osdOverlay.requestRedraw();
  m_colorPickerDialogPopup.requestRedraw();
  m_glyphPickerDialogPopup.requestRedraw();
  m_fileDialogPopup.requestRedraw();
}

void Application::onUpowerStateChangedForHooks() {
  if (m_upowerService == nullptr) {
    return;
  }
  for (const auto& event : m_batteryHookState.update(m_upowerService->state())) {
    if (event.env.empty()) {
      m_hookManager.fire(event.kind);
    } else {
      m_hookManager.fire(event.kind, event.env);
    }
  }
}

void Application::onNetworkStateChangedForEvents(const NetworkState& state, NetworkChangeOrigin origin) {
  if (!m_prevWirelessEnabledForEvents.has_value()) {
    m_prevWirelessEnabledForEvents = state.wirelessEnabled;
    return;
  }
  const bool prev = *m_prevWirelessEnabledForEvents;
  if (prev != state.wirelessEnabled) {
    if (origin != NetworkChangeOrigin::Noctalia) {
      m_osdOverlay.show(wifiOsdContent(state.wirelessEnabled));
    }
    if (state.wirelessEnabled) {
      m_hookManager.fire(HookKind::WifiEnabled);
    } else {
      m_hookManager.fire(HookKind::WifiDisabled);
    }
  }
  m_prevWirelessEnabledForEvents = state.wirelessEnabled;
}

void Application::onBluetoothStateChangedForEvents(const BluetoothState& state, BluetoothStateChangeOrigin origin) {
  if (!m_prevBluetoothPoweredForEvents.has_value()) {
    m_prevBluetoothPoweredForEvents = state.powered;
    return;
  }
  const bool prev = *m_prevBluetoothPoweredForEvents;
  if (prev != state.powered) {
    if (origin != BluetoothStateChangeOrigin::Noctalia) {
      m_osdOverlay.show(bluetoothOsdContent(state.powered));
    }
    if (state.powered) {
      m_hookManager.fire(HookKind::BluetoothEnabled);
    } else {
      m_hookManager.fire(HookKind::BluetoothDisabled);
    }
  }
  m_prevBluetoothPoweredForEvents = state.powered;
}

void Application::onPowerProfileChangedForEvents(const PowerProfilesState& state, PowerProfilesChangeOrigin origin) {
  if (state.activeProfile.empty()) {
    return;
  }
  if (!m_prevPowerProfileActiveForEvents.has_value()) {
    m_prevPowerProfileActiveForEvents = state.activeProfile;
    return;
  }
  const std::string prev = *m_prevPowerProfileActiveForEvents;
  if (prev != state.activeProfile) {
    if (origin != PowerProfilesChangeOrigin::Noctalia) {
      m_osdOverlay.show(powerProfileOsdContent(state.activeProfile));
    }
    m_hookManager.fire(
        HookKind::PowerProfileChanged,
        {{"NOCTALIA_POWER_PROFILE", state.activeProfile},
         {"NOCTALIA_POWER_PROFILE_PREVIOUS", prev},
         {"NOCTALIA_POWER_PROFILE_ORIGIN", std::string(powerProfileOriginName(origin))}}
    );
  }
  m_prevPowerProfileActiveForEvents = state.activeProfile;
}

std::vector<PollSource*> Application::currentPollSources() {
  std::vector<PollSource*> sources;
  if (m_busPollSource != nullptr) {
    sources.push_back(m_busPollSource.get());
  }
  if (m_screenSaverService != nullptr
      && m_screenSaverService->hasScreenSaverBus()
      && m_screenSaverPollSource != nullptr) {
    sources.push_back(m_screenSaverPollSource.get());
  }
  if (m_systemBusPollSource != nullptr) {
    sources.push_back(m_systemBusPollSource.get());
  }
  sources.push_back(&m_notificationPollSource);
  sources.push_back(&m_deferredCallPollSource);
  sources.push_back(&m_timePollSource);
  sources.push_back(&m_configPollSource);
  sources.push_back(&m_desktopEntryPollSource);
  sources.push_back(&m_iconThemePollSource);
  sources.push_back(&m_clipboardPollSource);
  sources.push_back(&m_timerPollSource);
  sources.push_back(&m_keyRepeatPollSource);
  sources.push_back(&m_workspacePollSource);
  sources.push_back(&m_keyboardLayoutPollSource);
  if constexpr (kLockKeysEnabled) {
    if (lockKeysConsumersEnabled(m_configService.config())) {
      sources.push_back(&m_lockKeysPollSource);
    }
  }
  if (m_pipewirePollSource != nullptr) {
    sources.push_back(m_pipewirePollSource.get());
  }
  if (m_pipewireSpectrumPollSource != nullptr) {
    sources.push_back(m_pipewireSpectrumPollSource.get());
  }
  if (m_polkitPollSource != nullptr) {
    sources.push_back(m_polkitPollSource.get());
  }
  if (m_brightnessPollSource != nullptr) {
    sources.push_back(m_brightnessPollSource.get());
  }
  sources.push_back(&m_fileWatchPollSource);
  sources.push_back(&m_ipcPollSource);
  sources.push_back(&m_httpClientPollSource);
  sources.push_back(&m_locationPollSource);
  sources.push_back(&m_weatherPollSource);
  sources.push_back(&m_calendarPollSource);
  sources.push_back(&m_thumbnailService);
  sources.push_back(&m_wallpaperScanner);
  sources.push_back(&m_asyncTextureCache);
  return sources;
}

std::vector<PollSource*> Application::buildPollSources() {
  if (m_bus != nullptr) {
    if (m_busPollSource == nullptr) {
      m_busPollSource = std::make_unique<SessionBusPollSource>(*m_bus);
    }
  } else {
    m_busPollSource.reset();
  }
  if (m_screenSaverService != nullptr && m_screenSaverService->hasScreenSaverBus()) {
    if (m_screenSaverPollSource == nullptr) {
      m_screenSaverPollSource = std::make_unique<ScreenSaverPollSource>(*m_screenSaverService);
    }
  } else {
    m_screenSaverPollSource.reset();
  }
  if (m_systemBus != nullptr) {
    if (m_systemBusPollSource == nullptr) {
      m_systemBusPollSource = std::make_unique<SystemBusPollSource>(*m_systemBus);
    }
  } else {
    m_systemBusPollSource.reset();
  }
  if (m_pipewireService != nullptr) {
    if (m_pipewirePollSource == nullptr) {
      m_pipewirePollSource = std::make_unique<PipeWirePollSource>(*m_pipewireService);
    }
  } else {
    m_pipewirePollSource.reset();
  }
  if (m_pipewireSpectrum != nullptr) {
    if (m_pipewireSpectrumPollSource == nullptr) {
      m_pipewireSpectrumPollSource = std::make_unique<PipeWireSpectrumPollSource>(*m_pipewireSpectrum);
    }
  } else {
    m_pipewireSpectrumPollSource.reset();
  }
  if (m_brightnessService != nullptr) {
    if (m_brightnessPollSource == nullptr) {
      m_brightnessPollSource = std::make_unique<BrightnessPollSource>(*m_brightnessService);
    }
  } else {
    m_brightnessPollSource.reset();
  }
  return currentPollSources();
}
