#pragma once

#include "config/config_types.h"
#include "shell/desktop/desktop_widget_services.h"
#include "ui/dialogs/layer_popup_host.h"

#include <memory>

class Bar;
class ConfigService;
class Dock;
class DesktopWidgetsController;
class IpcService;
class LockScreen;
class DesktopWidgetsEditor;
class LockscreenWidgetsHost;
class RenderContext;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;

using LockscreenWidgetsSnapshot = LockscreenWidgetsConfig;

struct LockscreenWidgetsControllerServices {
  DesktopWidgetServices widgets;
  LockScreen& lockScreen;
  Bar& bar;
  Dock& dock;
  DesktopWidgetsController* desktopWidgets = nullptr;
};

class LockscreenWidgetsController {
public:
  LockscreenWidgetsController();
  ~LockscreenWidgetsController();

  LockscreenWidgetsController(const LockscreenWidgetsController&) = delete;
  LockscreenWidgetsController& operator=(const LockscreenWidgetsController&) = delete;

  void initialize(const LockscreenWidgetsControllerServices& services);

  void registerIpc(IpcService& ipc);
  void onLockStateChanged();
  void onOutputChange();
  void onSecondTick();
  void requestLayout();
  void requestRedraw();

  void enterEdit();
  void exitEdit();
  void toggleEdit();

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

  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  LockScreen* m_lockScreen = nullptr;
  Bar* m_bar = nullptr;
  Dock* m_dock = nullptr;
  DesktopWidgetsController* m_desktopWidgets = nullptr;
  RenderContext* m_renderContext = nullptr;

  LockscreenWidgetsSnapshot m_snapshot;
  bool m_initialized = false;
  std::unique_ptr<LockscreenWidgetsHost> m_host;
  std::unique_ptr<DesktopWidgetsEditor> m_editor;
};
