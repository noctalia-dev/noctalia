#pragma once

#include "core/timer_manager.h"
#include "ipc/ipc_invocation_context.h"
#include "shell/bar/bar_instance.h"
#include "shell/bar/bar_services.h"
#include "shell/bar/widget_action_dispatcher.h"
#include "shell/bar/widget_factory.h"
#include "ui/dialogs/layer_popup_host.h"
#include "wayland/surface.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class TaskbarWidget;

class ConfigService;
class CompositorPlatform;
class FileWatcher;
class HttpClient;
class IdleInhibitor;
class IpcService;
class LockKeysService;
class ModemManagerService;
class MprisService;
class BluetoothService;
class BrightnessService;
class ClipboardService;
class EasyEffectsService;
class ExternalIpService;
class ScreenshotService;
class INetworkService;
class NotificationManager;
class PipeWireService;
class PipeWireSpectrum;
class PowerProfilesService;
class RenderContext;
class SystemMonitorService;
class UPowerService;
class TrayService;
class GammaService;
class WeatherService;
namespace noctalia::theme {
  class ThemeService;
}
namespace scripting {
  class ScriptApiContext;
}
struct PointerEvent;
struct wl_surface;

class Bar {
public:
  Bar();

  bool initialize(const BarServices& services);
  void reload();
  void closeAllInstances();
  void show();
  void hide();
  void toggle();
  /// Hides bars while a full-screen overlay editor (e.g. lockscreen widget layout) is active.
  void suppressDisplay();
  void unsuppressDisplay();
  bool isVisible() const noexcept;
  void onOutputChange();
  void onWorkspaceChanged();
  void scheduleSmartAutoHideReevaluation();
  void onSecondTick();
  // True when any bar widget renders a value that changes every second, so the
  // time source should wake on second boundaries.
  bool wantsSecondTicks() const;
  void refresh();
  void requestLayout();
  void setAutoHideSuppressionCallback(std::function<bool(const BarInstance&)> callback);
  // Re-run auto-hide after a panel closes so unrelated bars are not left visible.
  void reevaluateAutoHide();
  // A panel closed without the pointer moving (key press, etc.). Hover state is
  // unchanged, so no hover event will re-arm the tooltip for the widget the pointer
  // is still sitting on — do it here. No-op when the pointer is not on a bar.
  void rearmTooltipForHoveredWidget();
  // Grabbed popups often swallow Leave; resync pointerInside from the compositor
  // then re-run auto-hide (tray menus, same pattern as dock context menus).
  void reevaluateAutoHideAfterPopup();
  // Requests a redraw on every bar surface without re-running widget update/layout.
  // Intended for reactive restyling (palette changes) where the scene graph has
  // already been mutated in place and only a repaint is needed.
  void requestRedraw();
  bool onPointerEvent(const PointerEvent& event);
  bool isRunning() const noexcept;
  std::optional<LayerPopupParentContext> popupParentContextForSurface(wl_surface* surface) const noexcept;
  std::optional<LayerPopupParentContext> preferredPopupParentContext(wl_output* output) const noexcept;
  // Returns the bar surface rects on the given output in output-local logical
  // coordinates. Used by the panel click shield to keep clicks on bar widgets
  // flowing to the bar instead of dismissing the active panel.
  std::vector<InputRect> surfaceRectsForOutput(wl_output* output) const;
  // Returns every bar wl_surface across all outputs. Used as the focus-grab
  // whitelist on Hyprland so bar widgets keep receiving clicks.
  std::vector<wl_surface*> allBarSurfaces() const;
  // Bar surfaces that are visually shown and can anchor a Wayland idle inhibitor.
  std::vector<wl_surface*> caffeineAnchorSurfaces() const;
  void
  setAttachedPanelGeometry(wl_output* output, std::string_view barName, std::optional<AttachedPanelGeometry> geometry);
  bool canAttachPanelToBar(wl_output* output, std::string_view barName) const noexcept;
  std::optional<BarConfig> configForBar(wl_output* output, std::string_view barName) const;
  // Highest layer-shell layer occupied by any enabled bar on the given output (defaults to Top when no bar is present).
  // Hot corners use this to sit on the highest bar's layer so they are always activable.
  LayerShellLayer highestLayerForOutput(wl_output* output) const noexcept;
  // True when an attached panel may start its reveal animation: non-autohide bars, or autohide
  // bars that have finished sliding into their resting position.
  bool isAttachedPanelBarSettled(wl_output* output, std::string_view barName) const noexcept;
  void revealAutoHideForAttachedPanel(wl_output* output, std::string_view barName);
  void beginAttachedPopup(wl_surface* surface);
  void endAttachedPopup(wl_surface* surface);

  void registerIpc(IpcService& ipc);

private:
  void applyIpcVisibility(bool visible);
  void syncIdleInhibitorAnchors();
  void setInstanceIpcVisible(BarInstance& instance, bool visible);
  bool instanceEffectivelyVisible(const BarInstance& instance) const noexcept;
  static void tickWidgets(std::vector<std::unique_ptr<Widget>>& widgets, float deltaMs);
  static bool widgetsNeedFrameTick(const std::vector<std::unique_ptr<Widget>>& widgets);
  static bool instanceNeedsFrameTick(const BarInstance& instance);
  static bool widgetsWantSecondTicks(const std::vector<std::unique_ptr<Widget>>& widgets);
  static bool instanceWantsSecondTicks(const BarInstance& instance);
  void syncInstances();
  void createInstance(const WaylandOutput& output, std::size_t barIndex, const BarConfig& barConfig);
  void destroyInstance(std::uint32_t outputName);
  TaskbarWidget* findTaskbarWidget(const IpcInvocationContext& context) const;
  void populateWidgets(BarInstance& instance);
  void attachWidgetsToSections(BarInstance& instance);
  void updateWidgetHoverHighlight(BarInstance& instance, InputArea* hoveredArea);
  void animateWidgetHoverHighlight(BarInstance& instance, Widget& widget, bool hovered);
  void updateAccordionExpansion(BarInstance& instance, InputArea* hoveredArea);
  void beginAccordionAnimation(BarInstance& instance, BarCapsuleRun& run, bool expanded);
  void rebuildInstanceContents(BarInstance& instance, const BarConfig& newConfig);
  BarServices services() const;
  void buildScene(BarInstance& instance, std::uint32_t width, std::uint32_t height);
  void prepareFrame(BarInstance& instance, bool needsUpdate, bool needsLayout);
  void updateWidgets(BarInstance& instance);
  void applyBarCompositorBlur(BarInstance& instance) const;
  void syncBarSlideLayerTransform(BarInstance& instance) const;
  void syncBarAutoHideInputRegion(BarInstance& instance) const;
  void syncBarExclusiveZone(BarInstance& instance);
  void syncBarSurfaceChrome(BarInstance& instance);
  void clearInstancePointerState(BarInstance& instance);
  bool instanceAcceptsPointerInput(const BarInstance& instance) const noexcept;
  bool shouldReserveExclusiveZone(const BarInstance& instance) const noexcept;
  bool barContentVisuallyShown(const BarInstance& instance) const noexcept;
  void revealAutoHideBar(BarInstance& instance);
  bool isWorkspacePeekActive() const noexcept;
  void applyPendingWorkspaceReveal();
  void reevaluateSmartAutoHide();
  void startHideFadeOut(BarInstance& instance, bool immediate = false);
  static void applyBackgroundPalette(BarInstance& instance);
  std::string showBarIpc(std::string_view args);
  std::string hideBarIpc(std::string_view args);
  std::string toggleBarIpc(std::string_view args);
  std::string toggleBarReserveSpaceIpc(std::string_view args);
  std::string setBarAutoHideIpc(std::string_view args);
  std::string setBarLayerIpc(std::string_view args);
  [[nodiscard]] std::optional<std::string> collectBarIpcInstances(
      std::optional<std::string> barName, std::optional<std::string> monitorSelector,
      std::vector<BarInstance*>& instancesOut
  );
  BarInstance* instanceForSurface(wl_surface* surface) const noexcept;
  BarInstance* instanceForOutput(wl_output* output) const noexcept;
  BarInstance* instanceForBar(wl_output* output, std::string_view barName) const noexcept;

  CompositorPlatform* m_platform = nullptr;
  ConfigService* m_config = nullptr;
  NotificationManager* m_notifications = nullptr;
  TrayService* m_tray = nullptr;
  PipeWireService* m_audio = nullptr;
  EasyEffectsService* m_easyEffects = nullptr;
  UPowerService* m_upower = nullptr;
  SystemMonitorService* m_sysmon = nullptr;
  PowerProfilesService* m_powerProfiles = nullptr;
  INetworkService* m_network = nullptr;
  ModemManagerService* m_modem = nullptr;
  ExternalIpService* m_externalIp = nullptr;
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
  ScreenshotService* m_screenshots = nullptr;
  FileWatcher* m_fileWatcher = nullptr;
  scripting::ScriptApiContext* m_scriptApi = nullptr;
  std::unique_ptr<WidgetFactory> m_widgetFactory;
  std::vector<std::unique_ptr<BarInstance>> m_instances;
  // Wall-clock minute of the last full second-tick update, so minute-changing
  // widgets refresh on the first tick of a new minute even when it lands off
  // the :00 boundary (after suspend, a clock step, or the lock screen).
  std::chrono::sys_time<std::chrono::minutes> m_lastTickMinute;

  // Snapshot of the config fields the bar depends on. Used to skip reloads
  // triggered by unrelated config changes (theme, weather, idle, etc.).
  std::vector<BarConfig> m_lastBars;
  std::unordered_map<std::string, WidgetConfig> m_lastWidgets;
  ShellConfig::ShadowConfig m_lastShadow;
  // Plugin enable/disable changes which widget types resolve, so a plugins-only
  // config change must also rebuild widgets.
  PluginsConfig m_lastPlugins;

  // Surface → BarInstance mapping for pointer event routing
  std::unordered_map<wl_surface*, BarInstance*> m_surfaceMap;
  BarInstance* m_hoveredInstance = nullptr;
  std::function<bool(const BarInstance&)> m_autoHideSuppressionCallback;
  noctalia::bar::WidgetActionDispatcher m_actionDispatcher;
  Timer m_workspaceRevealDebounce;
  Timer m_workspacePeekHideTimer;
  std::unordered_set<std::uint32_t> m_pendingWorkspaceRevealOutputs;
  std::unordered_map<std::uint32_t, std::string> m_lastActiveWorkspaceByOutput;
  bool m_overlayDisplaySuppressed = false;
  bool m_wasVisibleBeforeOverlaySuppress = false;
  bool m_smartAutoHideReevalQueued = false;
};
