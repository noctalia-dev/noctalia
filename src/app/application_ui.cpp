#include "app/main_loop.h"
#include "application.h"
#include "application_internal.h"
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
#include "system/system_monitor_service.h"
#include "ui/app_icon_colorization.h"
#include "ui/controls/context_menu_popup.h"
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

namespace {
  constexpr Logger kLog("app");
} // namespace

void Application::initUi() {
  initUiRenderSurfacesAndSettings();
  initLockScreenAndSession();
  initInputDispatch();
  initPanelManagerAndPanels();
  initNotificationAndOsd();
  initBarDockAndLayout();
  initWidgetControllersAndCallbacks();
  // Wiring is complete and outputs are enumerated; build every per-output layer
  // surface once in canonical order. initialize() above only wired dependencies.
  reconcileOutputSurfaces();
}

void Application::initUiRenderSurfacesAndSettings() {

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
  m_settingsWindow.setSyncGreeterAppearance([this]() { performGreeterSync(); });
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
}

void Application::performGreeterSync(bool quiet) {
  if (!greeter::appearanceSyncAvailable(m_configService.config().shell.greeterSync)) {
    return;
  }

  const std::uint64_t generation = ++m_greeterSyncGeneration;
  m_greeterSyncTimeoutTimer.stop();

  const auto complete = [this, generation, quiet](bool success) {
    if (generation != m_greeterSyncGeneration) {
      return;
    }
    m_greeterSyncTimeoutTimer.stop();
    if (success) {
      if (!quiet) {
        DeferredCall::callLater([this]() {
          notify::info(
              "Noctalia", i18n::tr("notifications.internal.greeter-sync"),
              i18n::tr("notifications.internal.greeter-sync-success")
          );
        });
      }
      return;
    }
    DeferredCall::callLater([this, quiet]() {
      if (quiet) {
        notify::error(
            "Noctalia", i18n::tr("notifications.internal.greeter-sync"), i18n::tr("settings.errors.sync-greeter")
        );
      } else {
        m_settingsWindow.markSettingsWriteError(i18n::tr("settings.errors.sync-greeter"));
      }
    });
  };

  if (m_configService.config().shell.polkitAgent && m_polkitAgent != nullptr) {
    m_polkitAgent->markNextRequestInternal();
  }
  const auto launch = greeter::syncAppearanceToGreeterAsync(
      m_configService, m_themeService.resolvedMode(), complete, &m_compositorPlatform, m_logindService != nullptr
  );
  if (launch == greeter::GreeterSyncLaunch::Failed) {
    if (quiet) {
      notify::error(
          "Noctalia", i18n::tr("notifications.internal.greeter-sync"), i18n::tr("settings.errors.sync-greeter")
      );
    } else {
      m_settingsWindow.markSettingsWriteError(i18n::tr("settings.errors.sync-greeter"));
    }
    return;
  }
  if (launch == greeter::GreeterSyncLaunch::StagedOnly) {
    if (quiet) {
      notify::error(
          "Noctalia", i18n::tr("notifications.internal.greeter-sync"), i18n::tr("settings.errors.sync-greeter")
      );
    } else {
      notify::info(
          "Noctalia", i18n::tr("notifications.internal.greeter-sync"),
          i18n::tr("notifications.internal.greeter-sync-pending-manual")
      );
    }
    return;
  }

  if (!quiet) {
    const bool customPrivilege =
        !StringUtils::trim(m_configService.config().shell.greeterSync.privilegeCommand).empty();
    const bool polkitAgentActive = m_configService.config().shell.polkitAgent && m_polkitAgent != nullptr;
    const bool inSessionPolkit = likelySupportsInSessionPolkit();
    const char* pendingBodyKey = "notifications.internal.greeter-sync-pending";
    if (!customPrivilege && !polkitAgentActive && !inSessionPolkit) {
      pendingBodyKey = "notifications.internal.greeter-sync-pending-manual";
    } else if (!customPrivilege && !polkitAgentActive) {
      pendingBodyKey = "notifications.internal.greeter-sync-pending-console";
    }
    notify::info("Noctalia", i18n::tr("notifications.internal.greeter-sync"), i18n::tr(pendingBodyKey));
  }

  if (!quiet) {
    const bool inSessionPolkit = likelySupportsInSessionPolkit();
    m_greeterSyncTimeoutTimer.start(std::chrono::seconds(90), [this, generation, inSessionPolkit]() {
      if (generation != m_greeterSyncGeneration) {
        return;
      }
      DeferredCall::callLater([this, inSessionPolkit]() {
        notify::error(
            "Noctalia", i18n::tr("notifications.internal.greeter-sync"),
            i18n::tr(
                inSessionPolkit ? "notifications.internal.greeter-sync-timeout"
                                : "notifications.internal.greeter-sync-timeout-manual"
            )
        );
        m_settingsWindow.markSettingsWriteError(
            i18n::tr(
                inSessionPolkit ? "settings.errors.sync-greeter-timeout" : "settings.errors.sync-greeter-timeout-manual"
            )
        );
      });
    });
  }
}

void Application::scheduleGreeterAutoSync() {
  if (!m_configService.config().shell.greeterSync.autoSync
      || !greeter::appearanceSyncAvailable(m_configService.config().shell.greeterSync)) {
    return;
  }
  m_greeterAutoSyncTimer.stop();
  m_greeterAutoSyncTimer.start(std::chrono::milliseconds(1000), [this]() { performGreeterSync(true); });
}

void Application::initLockScreenAndSession() {
  m_lockScreen.initialize(
      m_wayland, &m_renderContext, &m_configService, &m_sharedTextureCache, m_systemBus.get(), &m_compositorPlatform
  );
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
        m_idleGraceOverlay.hide();
        m_lockscreenWidgetsController.onLockStateChanged();
        m_hookManager.fire(HookKind::SessionLocked);
      },
      [this]() {
        m_idleGraceOverlay.hide();
        m_lockscreenWidgetsController.onLockStateChanged();
        m_hookManager.fire(HookKind::SessionUnlocked);
        requestAllSurfacesRedraw();
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
}

void Application::initInputDispatch() {
  m_wayland.setPointerEventCallback([this](const PointerEvent& event) {
    if (m_lockScreen.isActive()) {
      m_lockScreen.onPointerEvent(event);
      return;
    }
    if (m_windowSwitcher.isActive()) {
      if (m_windowSwitcher.onPointerEvent(event)) {
        return;
      }
      if (event.type == PointerEvent::Type::Button || event.type == PointerEvent::Type::Axis) {
        return;
      }
      // Enter/Leave/Motion fall through so other surfaces' hover state stays in sync.
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
    if (m_panelManager.onPointerEvent(event))
      return;
    if (m_hotCorners.onPointerEvent(event))
      return;
    m_notificationToast.onPointerEvent(event);
  });

  m_wayland.setLockKeysChangeCallback([this]() {
    if (m_lockScreen.isActive()) {
      m_lockScreen.onLockKeysChanged();
    }
  });
  m_wayland.setKeyboardEventCallback([this](const KeyboardEvent& event) {
    if (m_lockScreen.isActive()) {
      m_lockScreen.onKeyboardEvent(event);
      return;
    }
    // Grab popups are modal — while one is open it owns the keyboard and ESC
    // dismisses it before anything behind can react.
    if (ContextMenuPopup::dispatchKeyboardEvent(event)) {
      return;
    }
    if (m_trayMenu.onKeyboardEvent(event)) {
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
}

void Application::initPanelManagerAndPanels() {
  // Panel manager must be before bar so widgets can access PanelManager::instance()
  m_panelManager.initialize(m_compositorPlatform, &m_configService, &m_renderContext);
  m_panelManager.setOpenSettingsWindowCallback([this](std::string context) {
    m_settingsWindow.open(std::move(context));
  });
  m_panelManager.setCloseSettingsWindowCallback([this]() { m_settingsWindow.close(); });
  m_panelManager.setToggleSettingsWindowCallback([this](std::string context) {
    if (m_settingsWindow.isOpen()) {
      m_settingsWindow.close();
      return;
    }
    m_settingsWindow.open(std::move(context));
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
    const ClipboardAutoPasteMode mode = m_configService.config().shell.clipboardAutoPaste;
    if (mode == ClipboardAutoPasteMode::Off) {
      m_panelManager.close();
      return;
    }
    // Auto-paste injects a keystroke into whatever holds keyboard focus. The animated close keeps
    // the panel surface (and its keyboard focus) alive for the duration of the reveal animation, so
    // the keys would land on the closing panel instead of the target window. Close without animation
    // so focus returns to the toplevel before we paste, mirroring the launcher's app-launch close.
    m_panelManager.closePanel(false);
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
          .asyncTextures = &m_asyncTextureCache,
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
  m_configService.addReloadCallback(
      [this]() {
        if (m_launcherPanel != nullptr) {
          m_launcherPanel->syncUsageTrackingState();
        }
      },
      "launcher-usage"
  );
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
  reloadDmenuProviders();
  reloadPluginPanels();
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
  m_compositorPlatform.setOverviewChangeCallback([this]() {
    m_overviewLauncherCapture.sync();
    m_bar.scheduleSmartAutoHideReevaluation();
    m_dock.scheduleSmartAutoHideReevaluation();
  });
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
}

void Application::initNotificationAndOsd() {
  m_notificationToast.initialize(m_wayland, &m_configService, &m_notificationManager, &m_renderContext, &m_httpClient);
  m_configService.addReloadCallback([this]() { m_notificationToast.onConfigReload(); });
  auto applyNotificationFilterConfig = [this]() {
    m_notificationManager.setFilters(m_configService.config().notification.filters);
  };
  applyNotificationFilterConfig();
  m_configService.addReloadCallback(applyNotificationFilterConfig);
  m_configService.setNotificationManager(&m_notificationManager);
  m_notificationManager.setSoundPlayer(m_soundPlayer.get());

  TooltipManager::instance().initialize(m_wayland, &m_configService, &m_renderContext);
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
      [this](bool userCancelled, bool willLockSession) {
        // Keep the overlay only when handing off to Noctalia's lock screen (avoids a flash).
        // External lockers never take ownership; deferred hide also races with suspend.
        const bool handoffToLockScreen = !userCancelled && willLockSession && m_configService.isLockScreenEnabled();
        if (!handoffToLockScreen) {
          m_idleGraceOverlay.hide();
        }
        if (userCancelled) {
          m_lockScreen.clearPrimedDesktopCaptures();
        }
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
  m_keyboardBacklightOsd.bindOverlay(m_osdOverlay);
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
}

void Application::initBarDockAndLayout() {
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
  m_panelManager.setAttachedPanelLayerProvider([this](wl_output* output, std::string_view barName) {
    return m_bar.layerForBar(output, barName);
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
  m_configService.addReloadCallback([this]() {
    if (m_configService.lastChange().shell) {
      reloadDmenuProviders();
    }
  });
  m_configService.addReloadCallback([this]() { m_screenCorners.onConfigReload(); });
  m_configService.addReloadCallback([this]() { m_hotCorners.onConfigReload(); });

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
}

void Application::initWidgetControllersAndCallbacks() {
  auto shouldRefreshControlCenter = [this]() { return m_panelManager.isOpenPanel("control-center"); };

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

  // Wire the corner surface owners here alongside the dock. Surface creation and
  // stacking order live entirely in reconcileOutputSurfaces(): screen corners and
  // the hot-corner trigger zones are built after the bar and dock so they are
  // never occluded by shell chrome on their shared layer.
  m_screenCorners.initialize(m_wayland, &m_configService, &m_renderContext);
  m_hotCorners.initialize(m_wayland, &m_configService, &m_renderContext);
}
