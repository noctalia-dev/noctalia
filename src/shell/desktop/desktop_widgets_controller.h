#pragma once

#include "config/config_types.h"
#include "shell/desktop/desktop_widget_services.h"
#include "ui/dialogs/layer_popup_host.h"

#include <cstdint>
#include <functional>
#include <memory>

class DesktopWidgetsEditor;
class DesktopWidgetsHost;
class LockscreenWidgetsController;
class IpcService;
class RenderContext;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;

using DesktopWidgetsSnapshot = DesktopWidgetsConfig;

struct DesktopWidgetsControllerServices {
  DesktopWidgetServices widgets;
  LockscreenWidgetsController* lockscreenWidgets = nullptr;
};

class DesktopWidgetsController {
public:
  DesktopWidgetsController();
  ~DesktopWidgetsController();

  DesktopWidgetsController(const DesktopWidgetsController&) = delete;
  DesktopWidgetsController& operator=(const DesktopWidgetsController&) = delete;

  void initialize(const DesktopWidgetsControllerServices& services);

  void registerIpc(IpcService& ipc);
  void onOutputChange();
  void onSecondTick();
  void requestUpdate();
  void requestLayout();
  void requestRedraw();

  void enterEdit();
  void exitEdit();
  void toggleEdit();
  void setOnEnterEditCallback(std::function<void()> callback);

  /// Hides on-screen desktop widgets while another overlay editor (e.g. lockscreen layout) is active.
  void suppressDisplay();
  void unsuppressDisplay();

  /// Ephemeral, IPC-driven runtime visibility override layered on top of the saved
  /// `desktop_widgets.enabled` setting (in the spirit of the bar's bar-show/bar-hide/bar-toggle, but
  /// bidirectional). The override is never persisted and resets to FollowConfig on restart, leaving
  /// the user's saved preference untouched. ForceShown reveals widgets even when the setting is
  /// disabled -- this is what lets opt-in workflows (saved default off, revealed on demand by e.g. a
  /// peek-desktop keybind) work without rewriting settings.toml. Hiding tears down the widget
  /// instances (applyVisibility -> host->hide()), so it also stops their rendering/compute.
  enum class RuntimeVisibility : std::uint8_t { FollowConfig, ForceShown, ForceHidden };
  void setRuntimeVisibility(RuntimeVisibility visibility);
  void toggleRuntimeVisibility();
  [[nodiscard]] bool isEffectivelyVisible() const noexcept;

  [[nodiscard]] bool isEditing() const noexcept;
  [[nodiscard]] std::optional<LayerPopupParentContext> popupParentContextForSurface(wl_surface* surface) const;
  [[nodiscard]] std::optional<LayerPopupParentContext> fallbackPopupParentContext() const;
  bool onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);

private:
  void loadSnapshotFromConfig();
  void saveSnapshotToConfig();
  void applyVisibility();
  void handleConfigReload();
  void normalizeSnapshot();
  [[nodiscard]] bool runtimeWantsVisible() const noexcept;

  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  LockscreenWidgetsController* m_lockscreenWidgets = nullptr;
  RenderContext* m_renderContext = nullptr;

  DesktopWidgetsSnapshot m_snapshot;
  bool m_initialized = false;
  bool m_displaySuppressed = false;
  RuntimeVisibility m_runtimeVisibility = RuntimeVisibility::FollowConfig;
  // Last-seen saved desktop_widgets.enabled; an explicit transition clears the runtime override.
  bool m_lastEnabled = false;
  std::function<void()> m_onEnterEdit;
  std::unique_ptr<DesktopWidgetsHost> m_host;
  std::unique_ptr<DesktopWidgetsEditor> m_editor;
};
