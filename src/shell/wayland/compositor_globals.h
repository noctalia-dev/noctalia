#pragma once

#include "wayland/wayland_extension.h"

#include <functional>

class WaylandConnection;
struct ext_workspace_manager_v1;
struct hyprland_toplevel_mapping_manager_v1;
struct org_kde_plasma_virtual_desktop_management;
struct zdwl_ipc_manager_v2;

// Workspace and window-mapping managers handed to CompositorPlatform: ext-workspace, KDE virtual desktops, dwl IPC
// and Hyprland toplevel mapping. Each bound manager goes to its callback, which takes ownership; a manager without a
// callback is destroyed at once, so install the callbacks before connect().
class CompositorGlobals final : public WaylandExtension {
public:
  explicit CompositorGlobals(WaylandConnection& connection);

  void onGlobal(wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version) override;
  void releaseGlobals() override;

  void setWorkspaceManagerCallbacks(
      std::function<void(ext_workspace_manager_v1*)> extWorkspace, std::function<void(zdwl_ipc_manager_v2*)> dwlIpc
  );
  void setKdeVirtualDesktopManagerCallback(std::function<void(org_kde_plasma_virtual_desktop_management*)> callback);
  void setHyprlandToplevelMappingManagerCallback(std::function<void(hyprland_toplevel_mapping_manager_v1*)> callback);

  bool hasExtWorkspaceManager() const noexcept { return m_hasExtWorkspaceGlobal; }
  bool hasKdeVirtualDesktopManager() const noexcept { return m_hasKdeVirtualDesktopGlobal; }
  bool hasDwlIpcManager() const noexcept { return m_hasDwlIpcGlobal; }

private:
  bool m_hasExtWorkspaceGlobal = false;
  bool m_hasKdeVirtualDesktopGlobal = false;
  bool m_hasDwlIpcGlobal = false;
  std::function<void(ext_workspace_manager_v1*)> m_extWorkspaceManagerCallback;
  std::function<void(org_kde_plasma_virtual_desktop_management*)> m_kdeVirtualDesktopManagerCallback;
  std::function<void(zdwl_ipc_manager_v2*)> m_dwlIpcManagerCallback;
  std::function<void(hyprland_toplevel_mapping_manager_v1*)> m_hyprlandToplevelMappingManagerCallback;
};
