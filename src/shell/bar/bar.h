#pragma once

#include "shell/bar/bar_instance.h"
#include "shell/bar/widget_factory.h"
#include "shell/panel/attached_panel_context.h"
#include "ui/dialogs/layer_popup_host.h"
#include "wayland/surface.h"

#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

class ConfigService;
class CompositorPlatform;
class FileWatcher;
class HttpClient;
class IdleInhibitor;
class IpcService;
class LockKeysService;
class MprisService;
class BluetoothService;
class BrightnessService;
class ClipboardService;
class INetworkService;
class NotificationManager;
class PipeWireService;
class PipeWireSpectrum;
class PowerProfilesService;
class RenderContext;
class SystemMonitorService;
class UPowerService;
class TimeService;
class TrayService;
class GammaService;
class WeatherService;
namespace noctalia::theme {
  class ThemeService;
}
struct PointerEvent;
struct wl_surface;

class Bar {
public:
  Bar();

  bool initialize(
      CompositorPlatform& platform, ConfigService* config, TimeService* timeService, NotificationManager* notifications,
      TrayService* tray, PipeWireService* audio, UPowerService* upower, SystemMonitorService* sysmon,
      PowerProfilesService* powerProfiles, INetworkService* network, IdleInhibitor* idleInhibitor, MprisService* mpris,
      PipeWireSpectrum* audioSpectrum, HttpClient* httpClient, WeatherService* weatherService,
      RenderContext* renderContext, GammaService* nightLight, noctalia::theme::ThemeService* themeService,
      BluetoothService* bluetooth, BrightnessService* brightness, LockKeysService* lockKeys,
      ClipboardService* clipboard, FileWatcher* fileWatcher = nullptr
  );
  void reload();
  void closeAllInstances();
  void show();
  void hide();
  void toggle();
  [[nodiscard]] bool isVisible() const noexcept;
  void onOutputChange();
  void onSecondTick();
  void refresh();
  void requestLayout();
  void setAutoHideSuppressionCallback(std::function<bool(const BarInstance&)> callback);
  // Re-run auto-hide after a panel closes so unrelated bars are not left visible.
  void reevaluateAutoHide();
  void setOpenWidgetSettingsCallback(std::function<void(std::string, std::string)> callback);
  // Requests a redraw on every bar surface without re-running widget update/layout.
  // Intended for reactive restyling (palette changes) where the scene graph has
  // already been mutated in place and only a repaint is needed.
  void requestRedraw();
  bool onPointerEvent(const PointerEvent& event);
  [[nodiscard]] bool isRunning() const noexcept;
  [[nodiscard]] std::optional<LayerPopupParentContext> popupParentContextForSurface(wl_surface* surface) const noexcept;
  [[nodiscard]] std::optional<LayerPopupParentContext> preferredPopupParentContext(wl_output* output) const noexcept;
  // Returns the bar surface rects on the given output in output-local logical
  // coordinates. Used by the panel click shield to keep clicks on bar widgets
  // flowing to the bar instead of dismissing the active panel.
  [[nodiscard]] std::vector<InputRect> surfaceRectsForOutput(wl_output* output) const;
  // Returns every bar wl_surface across all outputs. Used as the focus-grab
  // whitelist on Hyprland so bar widgets keep receiving clicks.
  [[nodiscard]] std::vector<wl_surface*> allBarSurfaces() const;
  void
  setAttachedPanelGeometry(wl_output* output, std::string_view barName, std::optional<AttachedPanelGeometry> geometry);
  void beginAttachedPopup(wl_surface* surface);
  void endAttachedPopup(wl_surface* surface);

  void registerIpc(IpcService& ipc);

private:
  void applyIpcVisibility(bool visible);
  void setInstanceIpcVisible(BarInstance& instance, bool visible);
  [[nodiscard]] bool instanceEffectivelyVisible(const BarInstance& instance) const noexcept;
  static void tickWidgets(std::vector<std::unique_ptr<Widget>>& widgets, float deltaMs);
  [[nodiscard]] static bool widgetsNeedFrameTick(const std::vector<std::unique_ptr<Widget>>& widgets);
  [[nodiscard]] static bool instanceNeedsFrameTick(const BarInstance& instance);
  void syncInstances();
  void createInstance(const WaylandOutput& output, std::size_t barIndex, const BarConfig& barConfig);
  void destroyInstance(std::uint32_t outputName);
  void populateWidgets(BarInstance& instance);
  void attachWidgetsToSections(BarInstance& instance);
  void rebuildInstanceContents(BarInstance& instance, const BarConfig& newConfig);
  void buildScene(BarInstance& instance, std::uint32_t width, std::uint32_t height);
  void prepareFrame(BarInstance& instance, bool needsUpdate, bool needsLayout);
  void updateWidgets(BarInstance& instance);
  void applyBarCompositorBlur(BarInstance& instance) const;
  void syncBarSlideLayerTransform(BarInstance& instance) const;
  void syncBarAutoHideInputRegion(BarInstance& instance) const;
  void syncBarExclusiveZone(BarInstance& instance);
  void syncBarSurfaceChrome(BarInstance& instance);
  void clearInstancePointerState(BarInstance& instance);
  [[nodiscard]] bool instanceAcceptsPointerInput(const BarInstance& instance) const noexcept;
  [[nodiscard]] bool shouldReserveExclusiveZone(const BarInstance& instance) const noexcept;
  [[nodiscard]] bool barContentVisuallyShown(const BarInstance& instance) const noexcept;
  void revealAutoHideBar(BarInstance& instance);
  void startHideFadeOut(BarInstance& instance);
  static void applyBackgroundPalette(BarInstance& instance);
  [[nodiscard]] std::string dispatchScriptedWidgetIpc(std::string_view args);
  [[nodiscard]] std::string setBarAutoHideIpc(std::string_view args);
  [[nodiscard]] BarInstance* instanceForSurface(wl_surface* surface) const noexcept;
  [[nodiscard]] BarInstance* instanceForOutput(wl_output* output) const noexcept;
  [[nodiscard]] BarInstance* instanceForBar(wl_output* output, std::string_view barName) const noexcept;

  CompositorPlatform* m_platform = nullptr;
  ConfigService* m_config = nullptr;
  NotificationManager* m_notifications = nullptr;
  TrayService* m_tray = nullptr;
  PipeWireService* m_audio = nullptr;
  UPowerService* m_upower = nullptr;
  SystemMonitorService* m_sysmon = nullptr;
  PowerProfilesService* m_powerProfiles = nullptr;
  INetworkService* m_network = nullptr;
  IdleInhibitor* m_idleInhibitor = nullptr;
  MprisService* m_mpris = nullptr;
  PipeWireSpectrum* m_audioSpectrum = nullptr;
  HttpClient* m_httpClient = nullptr;
  WeatherService* m_weatherService = nullptr;
  RenderContext* m_renderContext = nullptr;
  GammaService* m_nightLight = nullptr;
  noctalia::theme::ThemeService* m_themeService = nullptr;
  BluetoothService* m_bluetooth = nullptr;
  BrightnessService* m_brightness = nullptr;
  LockKeysService* m_lockKeys = nullptr;
  ClipboardService* m_clipboard = nullptr;
  FileWatcher* m_fileWatcher = nullptr;
  std::unique_ptr<WidgetFactory> m_widgetFactory;
  std::vector<std::unique_ptr<BarInstance>> m_instances;

  // Snapshot of the config fields the bar depends on. Used to skip reloads
  // triggered by unrelated config changes (theme, weather, idle, etc.).
  std::vector<BarConfig> m_lastBars;
  std::unordered_map<std::string, WidgetConfig> m_lastWidgets;
  ShellConfig::ShadowConfig m_lastShadow;

  // Surface → BarInstance mapping for pointer event routing
  std::unordered_map<wl_surface*, BarInstance*> m_surfaceMap;
  BarInstance* m_hoveredInstance = nullptr;
  std::function<bool(const BarInstance&)> m_autoHideSuppressionCallback;
  std::function<void(std::string, std::string)> m_openWidgetSettingsCallback;
};
