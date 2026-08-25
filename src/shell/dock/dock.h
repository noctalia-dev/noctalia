#pragma once

#include "config/config_types.h"
#include "system/desktop_entry.h"
#include "system/icon_resolver.h"
#include "ui/signal.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct wl_output;

class CompositorPlatform;
class ConfigService;
class IpcService;
class RenderContext;
struct PointerEvent;
struct WaylandOutput;
struct wl_surface;
struct zwlr_foreign_toplevel_handle_v1;

namespace shell::dock {
  struct DockInstance;
  struct DockItemAction;
  struct DockPopup;
} // namespace shell::dock

class Dock {
public:
  Dock();
  ~Dock();

  bool initialize(CompositorPlatform& platform, ConfigService* config, RenderContext* renderContext);
  void reload();
  void show();
  /// Tears down dock surfaces without changing config (e.g. lockscreen widget editor overlay).
  void suppressDisplay();
  void unsuppressDisplay();
  void onWorkspaceChanged();
  void scheduleSmartAutoHideReevaluation();
  void closeAllInstances();
  void onOutputChange();
  void refresh();
  void toggleVisibility();
  void requestLayout();
  void requestRedraw();
  bool onPointerEvent(const PointerEvent& event);

  void registerIpc(IpcService& ipc);

private:
  // Returns true if the item list was modified (triggers a rebuild).
  bool refreshPinnedAppsIfNeeded();
  void pruneCachedToplevelHandles();
  void syncInstances();
  void createInstance(const WaylandOutput& output);
  // Drop any references the dock keeps to an instance (surface map, hovered, popup owner)
  // before the instance is destroyed. Safe to call multiple times.
  void detachInstanceState(shell::dock::DockInstance& inst);
  bool syncInstanceModel(shell::dock::DockInstance& instance);
  void rebuildItems(shell::dock::DockInstance& instance);
  void updateVisuals(shell::dock::DockInstance& instance);
  void updateHoverZoomPointer(shell::dock::DockInstance& instance, float sceneX, float sceneY);
  void clearHoverZoomPointer(shell::dock::DockInstance& instance);
  void activateOrLaunchItem(shell::dock::DockInstance& instance, const shell::dock::DockItemAction& action);
  void tryFulfillPendingLaunchFocus();
  void openItemMenu(shell::dock::DockInstance& instance, const shell::dock::DockItemAction& action);
  void closeItemMenu();
  void beginDrag(shell::dock::DockInstance& instance, std::size_t index, float mainPos);
  void updateDrag(shell::dock::DockInstance& instance, float mainPos);
  void endDrag(shell::dock::DockInstance& instance, bool commit);
  void reevaluateSmartAutoHide();

  CompositorPlatform* m_platform = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  DockConfig m_lastDockConfig{};
  ShellConfig::ShadowConfig m_lastShadow;
  std::vector<std::string> m_lastPinnedConfig;
  std::vector<std::string> m_lastBarLayerStack;
  std::vector<DesktopEntry> m_pinnedEntries;
  std::uint64_t m_modelSerial = 0;
  std::uint64_t m_entriesVersion = 0;
  IconResolver m_iconResolver;
  struct PendingLaunchFocus {
    std::string idLower;
    std::string wmClassLower;
    wl_output* outputFilter = nullptr;
    // Launch target output (always set); outputFilter is only for active_monitor_only.
    wl_output* targetOutput = nullptr;
    std::chrono::steady_clock::time_point deadline;
  };

  std::unordered_map<std::string, zwlr_foreign_toplevel_handle_v1*> m_lastActiveHandleByAppIdLower;
  std::unordered_map<std::string, std::string> m_lastActiveIdentifierByAppIdLower;
  std::optional<PendingLaunchFocus> m_pendingLaunchFocus;
  std::vector<std::unique_ptr<shell::dock::DockInstance>> m_instances;
  std::unordered_map<wl_surface*, shell::dock::DockInstance*> m_surfaceMap;
  shell::dock::DockInstance* m_hoveredInstance = nullptr;
  shell::dock::DockInstance* m_popupOwnerInstance = nullptr; // instance that owns the current open popup
  std::unique_ptr<shell::dock::DockPopup> m_itemMenu;        // right-click context menu
  Signal<>::ScopedConnection m_appIconColorizeConn;
  bool m_overlayDisplaySuppressed = false;
  bool m_hadInstancesBeforeOverlaySuppress = false;
  bool m_smartAutoHideReevalQueued = false;
};
