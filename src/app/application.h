#pragma once

#include "app/deferred_call_poll_source.h"
#include "app/main_loop.h"
#include "app/timer_poll_source.h"
#include "calendar/calendar_poll_source.h"
#include "calendar/calendar_service.h"
#include "capture/screenshot_service.h"
#include "compositors/compositor_platform.h"
#include "config/config_poll_source.h"
#include "config/config_service.h"
#include "core/file_watcher.h"
#include "core/timer_manager.h"
#include "dbus/accounts/accounts_service.h"
#include "dbus/bluetooth/bluetooth_agent.h"
#include "dbus/bluetooth/bluetooth_service.h"
#include "dbus/idle/screensaver_poll_source.h"
#include "dbus/idle/screensaver_service.h"
#include "dbus/logind/logind_service.h"
#include "dbus/mpris/mpris_service.h"
#include "dbus/network/inetwork_service.h"
#include "dbus/network/network_secret_agent.h"
#include "dbus/notification/kde_notification_client.h"
#include "dbus/notification/notification_dbus_host.h"
#include "dbus/notification/notification_poll_source.h"
#include "dbus/notification/notification_service.h"
#include "dbus/polkit/polkit_agent.h"
#include "dbus/polkit/polkit_poll_source.h"
#include "dbus/power/power_profiles_service.h"
#include "dbus/session_bus.h"
#include "dbus/session_bus_poll_source.h"
#include "dbus/system_bus.h"
#include "dbus/system_bus_poll_source.h"
#include "dbus/tray/tray_service.h"
#include "dbus/upower/upower_service.h"
#include "debug/debug_service.h"
#include "hooks/battery_hook_state.h"
#include "hooks/hook_manager.h"
#include "idle/idle_grace_overlay.h"
#include "idle/idle_inhibitor.h"
#include "idle/idle_manager.h"
#include "ipc/ipc_poll_source.h"
#include "ipc/ipc_service.h"
#include "net/http_client.h"
#include "net/http_client_poll_source.h"
#include "notification/notification_manager.h"
#include "pipewire/pipewire_poll_source.h"
#include "pipewire/pipewire_service.h"
#include "pipewire/pipewire_spectrum.h"
#include "pipewire/pipewire_spectrum_poll_source.h"
#include "pipewire/sound_player.h"
#include "render/core/async_texture_cache.h"
#include "render/core/shared_texture_cache.h"
#include "render/core/thumbnail_service.h"
#include "render/gl_shared_context.h"
#include "render/render_context.h"
#include "scripting/plugin_ipc.h"
#include "scripting/plugin_manager.h"
#include "scripting/plugin_service_host.h"
#include "scripting/script_api_context.h"
#include "shell/backdrop/backdrop.h"
#include "shell/bar/bar.h"
#include "shell/desktop/desktop_widgets_controller.h"
#include "shell/dock/dock.h"
#include "shell/lockscreen/lock_screen.h"
#include "shell/lockscreen/lockscreen_widgets_controller.h"
#include "shell/notification/notification_toast.h"
#include "shell/osd/audio_osd.h"
#include "shell/osd/brightness_osd.h"
#include "shell/osd/keyboard_layout_osd.h"
#include "shell/osd/lock_keys_osd.h"
#include "shell/osd/media_osd.h"
#include "shell/osd/osd_overlay.h"
#include "shell/osd/privacy_osd.h"
#include "shell/overview/overview_launcher_capture.h"
#include "shell/panel/panel_manager.h"
#include "shell/polkit/polkit_panel.h"
#include "shell/screen_corners/screen_corners.h"
#include "shell/session/session_action_runner.h"
#include "shell/session/session_panel.h"
#include "shell/settings/settings_window.h"
#include "shell/switcher/window_switcher.h"
#include "shell/tray/tray_menu.h"
#include "shell/wallpaper/panel/wallpaper_scanner.h"
#include "shell/wallpaper/wallpaper.h"
#include "system/battery_warning_monitor.h"
#include "system/brightness_poll_source.h"
#include "system/brightness_service.h"
#include "system/dependency_service.h"
#include "system/desktop_entry_poll_source.h"
#include "system/easyeffects_service.h"
#include "system/gamma_service.h"
#include "system/icon_theme_poll_source.h"
#include "system/location_poll_source.h"
#include "system/location_service.h"
#include "system/lock_keys_poll_source.h"
#include "system/lock_keys_service.h"
#include "system/screen_time_service.h"
#include "system/system_monitor_service.h"
#include "system/telemetry_service.h"
#include "system/weather_poll_source.h"
#include "system/weather_service.h"
#include "theme/community_palettes.h"
#include "theme/community_templates.h"
#include "theme/template_apply_service.h"
#include "theme/theme_service.h"
#include "time/time_poll_source.h"
#include "time/time_service.h"
#include "ui/dialogs/color_picker_dialog_popup.h"
#include "ui/dialogs/file_dialog_popup.h"
#include "ui/dialogs/glyph_picker_dialog_popup.h"
#include "ui/dialogs/layer_popup_host.h"
#include "wayland/clipboard_poll_source.h"
#include "wayland/clipboard_service.h"
#include "wayland/key_repeat_poll_source.h"
#include "wayland/keyboard_layout_poll_source.h"
#include "wayland/text_input_service.h"
#include "wayland/virtual_keyboard_service.h"
#include "wayland/wayland_connection.h"
#include "wayland/workspace_poll_source.h"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace sdbus {
  class IProxy;
}

class LauncherPanel;

class Application {
public:
  Application();
  ~Application();

  void run(std::function<void()> startupReadyCallback = {});

  // Public for signal handler
  static std::atomic<bool> s_shutdownRequested;

private:
  void initServices();
  void initUi();
  void initIpc();
  // (Re)register plugin-backed launcher providers from the enabled plugin set.
  void reloadPluginLauncherProviders();
  void startTrayService();
  void syncNotificationDaemon();
  void installNotificationBusNameWatch();
  void scheduleNotificationShellRefresh();
  void syncPolkitAgent();
  void syncClipboardService();
  void syncScreenTimeService();
  bool runUserCommand(const std::string& command);
  bool runUserCommandBlocking(const std::string& command);
  bool runIdleAction(const IdleActionRequest& action);
  void onIconThemeChanged();
  void onGraphicsReset(RenderGraphicsResetStatus status);
  void requestAllSurfacesRedraw();
  void onUpowerStateChangedForHooks();
  void onNetworkStateChangedForEvents(const NetworkState& state, NetworkChangeOrigin origin);
  void onBluetoothStateChangedForEvents(const BluetoothState& state, BluetoothStateChangeOrigin origin);
  void onPowerProfileChangedForEvents(const PowerProfilesState& state, PowerProfilesChangeOrigin origin);
  [[nodiscard]] std::vector<PollSource*> currentPollSources();
  [[nodiscard]] std::vector<PollSource*> buildPollSources();

  WaylandConnection m_wayland;
  CompositorPlatform m_compositorPlatform{m_wayland};
  ClipboardService m_clipboardService;
  TextInputService m_textInputService;
  VirtualKeyboardService m_virtualKeyboardService;
  ConfigService m_configService;
  HttpClient m_httpClient;
  FileWatcher m_fileWatcher;
  noctalia::theme::CommunityPaletteService m_communityPaletteService{m_httpClient};
  noctalia::theme::CommunityTemplateService m_communityTemplateService{m_httpClient};
  noctalia::theme::ThemeService m_themeService{m_configService, m_httpClient};
  noctalia::theme::TemplateApplyService m_templateApplyService{m_configService};
  scripting::ScriptApiContext m_scriptApi;
  scripting::PluginManager m_pluginManager{m_configService};
  scripting::PluginServiceHost m_pluginServiceHost{m_scriptApi, &m_httpClient, &m_clipboardService, &m_fileWatcher};
  TimeService m_timeService;
  LockKeysService m_lockKeysService;
  NotificationManager m_notificationManager;
  std::unique_ptr<SessionBus> m_bus;
  std::unique_ptr<SystemBus> m_systemBus;
  std::unique_ptr<LogindService> m_logindService;
  std::unique_ptr<AccountsService> m_accountsService;
  std::unique_ptr<ScreenSaverService> m_screenSaverService;
  std::unique_ptr<ScreenSaverPollSource> m_screenSaverPollSource;
  std::unique_ptr<SystemMonitorService> m_systemMonitor;
  std::unique_ptr<DebugService> m_debugService;
  IdleInhibitor m_idleInhibitor;
  IdleManager m_idleManager;
  IdleGraceOverlay m_idleGraceOverlay;
  HookManager m_hookManager;
  DependencyService m_dependencyService;
  GammaService m_gammaService;
  ScreenshotService m_screenshotService{m_wayland, m_compositorPlatform, m_notificationManager, &m_clipboardService};
  std::unique_ptr<MprisService> m_mprisService;
  std::unique_ptr<PowerProfilesService> m_powerProfilesService;
  std::unique_ptr<INetworkService> m_networkService;
  std::unique_ptr<NetworkSecretAgent> m_networkSecretAgent;
  std::unique_ptr<BluetoothService> m_bluetoothService;
  std::unique_ptr<BluetoothAgent> m_bluetoothAgent;
  Timer m_bluetoothResumeTimer;
  std::unique_ptr<PolkitAgent> m_polkitAgent;
  std::unique_ptr<UPowerService> m_upowerService;
  std::optional<bool> m_notificationDaemonEnabled;
  bool m_notificationDaemonInitFailed = false;
  bool m_notificationShellRefreshScheduled = false;
  BatteryHookState m_batteryHookState;
  BatteryWarningMonitor m_batteryWarningMonitor;
  std::optional<bool> m_prevWirelessEnabledForEvents;
  std::optional<bool> m_prevBluetoothPoweredForEvents;
  std::optional<std::string> m_prevPowerProfileActiveForEvents;
  std::unique_ptr<BrightnessService> m_brightnessService;
  std::unique_ptr<TrayService> m_trayService;
  std::unique_ptr<NotificationDBusHost> m_notificationDbus;
  std::unique_ptr<sdbus::IProxy> m_notificationBusNameWatchProxy;
  bool m_notificationBusNameWatchInstalled = false;
  std::unique_ptr<PipeWireService> m_pipewireService;
  std::unique_ptr<EasyEffectsService> m_easyEffectsService;
  std::unique_ptr<PipeWireSpectrum> m_pipewireSpectrum;
  std::unique_ptr<SoundPlayer> m_soundPlayer;

  TelemetryService m_telemetryService;
  ScreenTimeService m_screenTimeService;

  GlSharedContext m_glShared;
  SharedTextureCache m_sharedTextureCache;
  RenderContext m_renderContext;
  ThumbnailService m_thumbnailService;
  WallpaperScanner m_wallpaperScanner;
  Bar m_bar;
  Dock m_dock;
  DesktopWidgetsController m_desktopWidgetsController;
  LockScreen m_lockScreen;
  LockscreenWidgetsController m_lockscreenWidgetsController;
  SessionActionRunner m_sessionActionRunner{m_compositorPlatform, m_lockScreen};
  PanelManager m_panelManager;
  // Owned by m_panelManager; kept raw so plugin launcher providers can be re-applied.
  LauncherPanel* m_launcherPanel = nullptr;
  WindowSwitcher m_windowSwitcher;
  OverviewLauncherCapture m_overviewLauncherCapture;
  NotificationToast m_notificationToast;
  AudioOsd m_audioOsd;
  BrightnessOsd m_brightnessOsd;
  MediaOsd m_mediaOsd;
  LockKeysOsd m_lockKeysOsd;
  KeyboardLayoutOsd m_keyboardLayoutOsd;
  PrivacyOsd m_privacyOsd;
  OsdOverlay m_osdOverlay;
  ScreenCorners m_screenCorners;
  TrayMenu m_trayMenu;
  Wallpaper m_wallpaper;
  Backdrop m_backdrop;
  SettingsWindow m_settingsWindow;
  LayerPopupHostRegistry m_layerPopupHosts;
  ColorPickerDialogPopup m_colorPickerDialogPopup;
  GlyphPickerDialogPopup m_glyphPickerDialogPopup;
  FileDialogPopup m_fileDialogPopup;
  AsyncTextureCache m_asyncTextureCache;

  // Poll sources (must outlive MainLoop)
  std::unique_ptr<SessionBusPollSource> m_busPollSource;
  std::unique_ptr<SystemBusPollSource> m_systemBusPollSource;
  NotificationPollSource m_notificationPollSource{m_notificationManager};
  DeferredCallPollSource m_deferredCallPollSource;
  TimePollSource m_timePollSource{m_timeService};
  ConfigPollSource m_configPollSource{m_configService};
  DesktopEntryPollSource m_desktopEntryPollSource;
  IconThemePollSource m_iconThemePollSource;
  ClipboardPollSource m_clipboardPollSource{m_clipboardService};
  TimerPollSource m_timerPollSource;
  KeyRepeatPollSource m_keyRepeatPollSource{m_wayland};
  WorkspacePollSource m_workspacePollSource{m_compositorPlatform};
  KeyboardLayoutPollSource m_keyboardLayoutPollSource{m_compositorPlatform};
  LockKeysPollSource m_lockKeysPollSource{m_lockKeysService};
  std::unique_ptr<BrightnessPollSource> m_brightnessPollSource;
  std::unique_ptr<PipeWirePollSource> m_pipewirePollSource;
  std::unique_ptr<PipeWireSpectrumPollSource> m_pipewireSpectrumPollSource;
  std::unique_ptr<PolkitPollSource> m_polkitPollSource;
  IpcService m_ipcService;
  IpcPollSource m_ipcPollSource{m_ipcService};
  LocationService m_locationService;
  WeatherService m_weatherService;
  CalendarService m_calendarService;
  HttpClientPollSource m_httpClientPollSource{m_httpClient};
  FileWatchPollSource m_fileWatchPollSource{m_fileWatcher};
  LocationPollSource m_locationPollSource{m_locationService};
  WeatherPollSource m_weatherPollSource{m_weatherService};
  CalendarPollSource m_calendarPollSource{m_calendarService};
  Timer m_trayInitTimer;
  Timer m_polkitInitTimer;
  Timer m_clipboardAutoPasteTimer;

  std::unique_ptr<MainLoop> m_mainLoop;
};
