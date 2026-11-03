#include "shell/wayland/compositor_globals.h"

#include "dwl-ipc-unstable-v2-client-protocol.h"
#include "ext-workspace-v1-client-protocol.h"
#include "hyprland-toplevel-mapping-v1-client-protocol.h"
#include "org-kde-plasma-virtual-desktop-client-protocol.h"

#include <algorithm>
#include <utility>

namespace {

  constexpr std::uint32_t kExtWorkspaceManagerVersion = 1;
  constexpr std::uint32_t kKdeVirtualDesktopManagementVersion = 4;
  constexpr std::uint32_t kDwlIpcManagerVersion = 2;
  constexpr std::uint32_t kHyprlandToplevelMappingManagerVersion = 1;

} // namespace

CompositorGlobals::CompositorGlobals(WaylandConnection& /*connection*/) {}

void CompositorGlobals::onGlobal(
    wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version
) {
  if (interface == ext_workspace_manager_v1_interface.name) {
    m_hasExtWorkspaceGlobal = true;
    const auto bindVersion = std::min(version, kExtWorkspaceManagerVersion);
    auto* manager = static_cast<ext_workspace_manager_v1*>(
        wl_registry_bind(registry, name, &ext_workspace_manager_v1_interface, bindVersion)
    );
    if (m_extWorkspaceManagerCallback) {
      m_extWorkspaceManagerCallback(manager);
    } else {
      ext_workspace_manager_v1_destroy(manager);
    }
    return;
  }

  if (interface == org_kde_plasma_virtual_desktop_management_interface.name) {
    m_hasKdeVirtualDesktopGlobal = true;
    const auto bindVersion = std::min(version, kKdeVirtualDesktopManagementVersion);
    auto* manager = static_cast<org_kde_plasma_virtual_desktop_management*>(
        wl_registry_bind(registry, name, &org_kde_plasma_virtual_desktop_management_interface, bindVersion)
    );
    if (m_kdeVirtualDesktopManagerCallback) {
      m_kdeVirtualDesktopManagerCallback(manager);
    } else {
      org_kde_plasma_virtual_desktop_management_destroy(manager);
    }
    return;
  }

  if (interface == zdwl_ipc_manager_v2_interface.name) {
    m_hasDwlIpcGlobal = true;
    auto* manager = static_cast<zdwl_ipc_manager_v2*>(
        wl_registry_bind(registry, name, &zdwl_ipc_manager_v2_interface, kDwlIpcManagerVersion)
    );
    if (m_dwlIpcManagerCallback) {
      m_dwlIpcManagerCallback(manager);
    } else {
      zdwl_ipc_manager_v2_release(manager);
    }
    return;
  }

  if (interface == hyprland_toplevel_mapping_manager_v1_interface.name) {
    const auto bindVersion = std::min(version, kHyprlandToplevelMappingManagerVersion);
    auto* manager = static_cast<hyprland_toplevel_mapping_manager_v1*>(
        wl_registry_bind(registry, name, &hyprland_toplevel_mapping_manager_v1_interface, bindVersion)
    );
    if (m_hyprlandToplevelMappingManagerCallback) {
      m_hyprlandToplevelMappingManagerCallback(manager);
    } else {
      hyprland_toplevel_mapping_manager_v1_destroy(manager);
    }
  }
}

void CompositorGlobals::releaseGlobals() {
  m_hasExtWorkspaceGlobal = false;
  m_hasDwlIpcGlobal = false;
  m_extWorkspaceManagerCallback = nullptr;
  m_kdeVirtualDesktopManagerCallback = nullptr;
  m_dwlIpcManagerCallback = nullptr;
}

void CompositorGlobals::setWorkspaceManagerCallbacks(
    std::function<void(ext_workspace_manager_v1*)> extWorkspace, std::function<void(zdwl_ipc_manager_v2*)> dwlIpc
) {
  m_extWorkspaceManagerCallback = std::move(extWorkspace);
  m_dwlIpcManagerCallback = std::move(dwlIpc);
}

void CompositorGlobals::setKdeVirtualDesktopManagerCallback(
    std::function<void(org_kde_plasma_virtual_desktop_management*)> callback
) {
  m_kdeVirtualDesktopManagerCallback = std::move(callback);
}

void CompositorGlobals::setHyprlandToplevelMappingManagerCallback(
    std::function<void(hyprland_toplevel_mapping_manager_v1*)> callback
) {
  m_hyprlandToplevelMappingManagerCallback = std::move(callback);
}
