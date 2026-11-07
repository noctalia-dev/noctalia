#include "shell/wayland/foreign_toplevels.h"

#include "compositors/compositor_detect.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "shell/wayland/wayland_protocol_policy.h"
#include "wayland/wayland_connection.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

#include <algorithm>

namespace {

  constexpr std::uint32_t kWlrForeignToplevelManagerVersion = 3;
  constexpr std::uint32_t kExtForeignToplevelListVersion = 1;

} // namespace

ForeignToplevels::ForeignToplevels(WaylandConnection& connection) : m_connection(connection) {}

void ForeignToplevels::onGlobal(
    wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version
) {
  if (interface == zwlr_foreign_toplevel_manager_v1_interface.name) {
    m_hasWlrManagerGlobal = true;
    const auto bindVersion = std::min(version, kWlrForeignToplevelManagerVersion);
    auto* manager = static_cast<zwlr_foreign_toplevel_manager_v1*>(
        wl_registry_bind(registry, name, &zwlr_foreign_toplevel_manager_v1_interface, bindVersion)
    );
    m_wlr.bind(manager);
    return;
  }

  if (interface == ext_foreign_toplevel_list_v1_interface.name) {
    if (!wayland_protocol_policy::shouldBindExtForeignToplevelList(compositors::detect())) {
      return;
    }
    m_hasExtListGlobal = true;
    const auto bindVersion = std::min(version, kExtForeignToplevelListVersion);
    auto* list = static_cast<ext_foreign_toplevel_list_v1*>(
        wl_registry_bind(registry, name, &ext_foreign_toplevel_list_v1_interface, bindVersion)
    );
    m_ext.bind(list, m_connection.display());
  }
}

void ForeignToplevels::releaseGlobals() {
  m_wlr.cleanup();
  m_ext.cleanup();
  m_hasWlrManagerGlobal = false;
}

void ForeignToplevels::setChangeCallback(ChangeCallback callback) {
  m_wlr.setChangeCallback(callback);
  m_ext.setChangeCallback(std::move(callback));
}

std::optional<ActiveToplevel> ForeignToplevels::activeToplevel() const { return m_wlr.current(); }

std::optional<ActiveToplevel> ForeignToplevels::matchToplevelByTitleAndAppId(
    std::string_view title, std::string_view appId, wl_output* preferredOutput
) const {
  return m_wlr.matchByTitleAndAppId(title, appId, preferredOutput);
}

wl_output* ForeignToplevels::activeToplevelOutput() const { return m_wlr.currentOutput(); }

bool ForeignToplevels::hasFullscreenToplevel() const { return m_wlr.anyFullscreen(); }

bool ForeignToplevels::usesExtForAppQueries() const {
  return compositors::isKde() && m_ext.isBound() && !m_hasWlrManagerGlobal;
}

std::vector<std::string> ForeignToplevels::runningAppIds(wl_output* outputFilter) const {
  if (usesExtForAppQueries()) {
    return m_ext.allAppIds();
  }
  return m_wlr.allAppIds(outputFilter);
}

std::vector<ToplevelInfo> ForeignToplevels::windowsForApp(
    const std::string& idLower, const std::string& wmClassLower, wl_output* outputFilter
) const {
  if (usesExtForAppQueries()) {
    return m_ext.windowsForApp(idLower, wmClassLower);
  }
  return m_wlr.windowsForApp(idLower, wmClassLower, outputFilter);
}

std::vector<ToplevelInfo> ForeignToplevels::windowsWithoutAppId(wl_output* outputFilter) const {
  if (usesExtForAppQueries()) {
    return m_ext.windowsWithoutAppId();
  }
  return m_wlr.windowsWithoutAppId(outputFilter);
}

std::vector<ToplevelInfo>
ForeignToplevels::extWindowsForApp(const std::string& idLower, const std::string& wmClassLower) const {
  if (!m_ext.isBound()) {
    return {};
  }
  return m_ext.windowsForApp(idLower, wmClassLower);
}

std::vector<ToplevelInfo> ForeignToplevels::extWindowsWithoutAppId() const {
  return m_ext.isBound() ? m_ext.windowsWithoutAppId() : std::vector<ToplevelInfo>{};
}

ext_foreign_toplevel_handle_v1* ForeignToplevels::extHandleForIdentifier(std::string_view identifier) const {
  return m_ext.isBound() ? m_ext.extHandleForIdentifier(identifier) : nullptr;
}

bool ForeignToplevels::containsWlrToplevelHandle(zwlr_foreign_toplevel_handle_v1* handle) const {
  return m_wlr.containsWlrHandle(handle);
}

void ForeignToplevels::activateToplevel(zwlr_foreign_toplevel_handle_v1* handle) {
  m_wlr.activateHandle(handle, m_connection.seat());
}

bool ForeignToplevels::minimizeToplevel(zwlr_foreign_toplevel_handle_v1* handle) {
  return m_wlr.minimizeHandle(handle);
}

std::optional<bool> ForeignToplevels::toplevelMinimized(zwlr_foreign_toplevel_handle_v1* handle) const {
  return m_wlr.minimized(handle);
}

void ForeignToplevels::closeToplevel(zwlr_foreign_toplevel_handle_v1* handle) { m_wlr.closeHandle(handle); }

void ForeignToplevels::setToplevelRectangle(
    zwlr_foreign_toplevel_handle_v1* handle, wl_surface* surface, std::int32_t x, std::int32_t y, std::int32_t width,
    std::int32_t height
) {
  m_wlr.setRectangle(handle, surface, x, y, width, height);
}
