#include "compositors/hyprland/hyprland_toplevel_mapping.h"

#include "compositors/hyprland/hyprland_window_id.h"
#include "core/log.h"
#include "hyprland-toplevel-mapping-v1-client-protocol.h"

#include <vector>

namespace compositors::hyprland {

  namespace {

    constexpr Logger kLog("hyprland_toplevel_mapping");

    const hyprland_toplevel_window_mapping_handle_v1_listener kMappingListener = {
        .window_address = &HyprlandToplevelMapping::handleWindowAddress,
        .failed = &HyprlandToplevelMapping::handleFailed,
    };

  } // namespace

  void HyprlandToplevelMapping::initialize(hyprland_toplevel_mapping_manager_v1* manager) { m_manager = manager; }

  void HyprlandToplevelMapping::cleanup() {
    for (auto& [_, request] : m_pending) {
      clearRequest(request);
    }
    m_pending.clear();
    m_windowIdByWlrHandle.clear();
    m_windowIdByExtHandle.clear();
    m_wlrHandleByWindowId.clear();
    m_extHandleByWindowId.clear();

    if (m_manager != nullptr) {
      hyprland_toplevel_mapping_manager_v1_destroy(m_manager);
      m_manager = nullptr;
    }
  }

  void HyprlandToplevelMapping::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

  void HyprlandToplevelMapping::syncWlrHandles(const std::vector<zwlr_foreign_toplevel_handle_v1*>& handles) {
    if (m_manager == nullptr) {
      return;
    }

    std::unordered_map<zwlr_foreign_toplevel_handle_v1*, bool> seen;
    seen.reserve(handles.size());
    for (auto* handle : handles) {
      if (handle == nullptr) {
        continue;
      }
      seen.emplace(handle, true);
      if (!m_windowIdByWlrHandle.contains(handle)) {
        requestWlrMapping(handle);
      }
    }

    std::vector<zwlr_foreign_toplevel_handle_v1*> stale;
    for (const auto& [handle, windowId] : m_windowIdByWlrHandle) {
      (void)windowId;
      if (!seen.contains(handle)) {
        stale.push_back(handle);
      }
    }
    for (auto* handle : stale) {
      if (const auto it = m_windowIdByWlrHandle.find(handle); it != m_windowIdByWlrHandle.end()) {
        m_wlrHandleByWindowId.erase(it->second);
        m_windowIdByWlrHandle.erase(it);
      }
    }

    std::vector<hyprland_toplevel_window_mapping_handle_v1*> staleRequests;
    for (auto& [requestHandle, request] : m_pending) {
      if (request.kind == TargetKind::Wlr && request.wlrToplevel != nullptr && !seen.contains(request.wlrToplevel)) {
        staleRequests.push_back(requestHandle);
      }
    }
    for (auto* requestHandle : staleRequests) {
      const auto it = m_pending.find(requestHandle);
      if (it != m_pending.end()) {
        clearRequest(it->second);
        m_pending.erase(it);
      }
    }
  }

  void HyprlandToplevelMapping::syncExtHandles(const std::vector<ext_foreign_toplevel_handle_v1*>& handles) {
    if (m_manager == nullptr) {
      return;
    }

    std::unordered_map<ext_foreign_toplevel_handle_v1*, bool> seen;
    seen.reserve(handles.size());
    for (auto* handle : handles) {
      if (handle == nullptr) {
        continue;
      }
      seen.emplace(handle, true);
      if (!m_windowIdByExtHandle.contains(handle)) {
        requestExtMapping(handle);
      }
    }

    std::vector<ext_foreign_toplevel_handle_v1*> stale;
    for (const auto& [handle, windowId] : m_windowIdByExtHandle) {
      (void)windowId;
      if (!seen.contains(handle)) {
        stale.push_back(handle);
      }
    }
    for (auto* handle : stale) {
      if (const auto it = m_windowIdByExtHandle.find(handle); it != m_windowIdByExtHandle.end()) {
        m_extHandleByWindowId.erase(it->second);
        m_windowIdByExtHandle.erase(it);
      }
    }

    std::vector<hyprland_toplevel_window_mapping_handle_v1*> staleRequests;
    for (auto& [requestHandle, request] : m_pending) {
      if (request.kind == TargetKind::Ext && request.extToplevel != nullptr && !seen.contains(request.extToplevel)) {
        staleRequests.push_back(requestHandle);
      }
    }
    for (auto* requestHandle : staleRequests) {
      const auto it = m_pending.find(requestHandle);
      if (it != m_pending.end()) {
        clearRequest(it->second);
        m_pending.erase(it);
      }
    }
  }

  std::optional<std::string>
  HyprlandToplevelMapping::windowIdForWlrHandle(zwlr_foreign_toplevel_handle_v1* handle) const {
    if (handle == nullptr) {
      return std::nullopt;
    }
    const auto it = m_windowIdByWlrHandle.find(handle);
    if (it == m_windowIdByWlrHandle.end() || it->second.empty()) {
      return std::nullopt;
    }
    return it->second;
  }

  std::optional<std::string>
  HyprlandToplevelMapping::windowIdForExtHandle(ext_foreign_toplevel_handle_v1* handle) const {
    if (handle == nullptr) {
      return std::nullopt;
    }
    const auto it = m_windowIdByExtHandle.find(handle);
    if (it == m_windowIdByExtHandle.end() || it->second.empty()) {
      return std::nullopt;
    }
    return it->second;
  }

  zwlr_foreign_toplevel_handle_v1*
  HyprlandToplevelMapping::wlrHandleForWindowId(const std::string_view windowId) const {
    const auto normalized = normalizeWindowId(windowId);
    if (normalized.empty()) {
      return nullptr;
    }
    const auto it = m_wlrHandleByWindowId.find(normalized);
    return it != m_wlrHandleByWindowId.end() ? it->second : nullptr;
  }

  ext_foreign_toplevel_handle_v1* HyprlandToplevelMapping::extHandleForWindowId(const std::string_view windowId) const {
    const auto normalized = normalizeWindowId(windowId);
    if (normalized.empty()) {
      return nullptr;
    }
    const auto it = m_extHandleByWindowId.find(normalized);
    return it != m_extHandleByWindowId.end() ? it->second : nullptr;
  }

  bool HyprlandToplevelMapping::isWindowIdKnown(const std::string_view windowId) const {
    return wlrHandleForWindowId(windowId) != nullptr || extHandleForWindowId(windowId) != nullptr;
  }

  void HyprlandToplevelMapping::requestWlrMapping(zwlr_foreign_toplevel_handle_v1* handle) {
    if (m_manager == nullptr || handle == nullptr) {
      return;
    }

    for (const auto& [_, request] : m_pending) {
      if (request.kind == TargetKind::Wlr && request.wlrToplevel == handle) {
        return;
      }
    }

    auto* requestHandle = hyprland_toplevel_mapping_manager_v1_get_window_for_toplevel_wlr(m_manager, handle);
    if (requestHandle == nullptr) {
      kLog.warn("get_window_for_toplevel_wlr returned null");
      return;
    }

    MappingRequest request{
        .request = requestHandle,
        .kind = TargetKind::Wlr,
        .wlrToplevel = handle,
    };
    hyprland_toplevel_window_mapping_handle_v1_add_listener(requestHandle, &kMappingListener, this);
    m_pending.emplace(requestHandle, request);
  }

  void HyprlandToplevelMapping::requestExtMapping(ext_foreign_toplevel_handle_v1* handle) {
    if (m_manager == nullptr || handle == nullptr) {
      return;
    }

    for (const auto& [_, request] : m_pending) {
      if (request.kind == TargetKind::Ext && request.extToplevel == handle) {
        return;
      }
    }

    auto* requestHandle = hyprland_toplevel_mapping_manager_v1_get_window_for_toplevel(m_manager, handle);
    if (requestHandle == nullptr) {
      kLog.warn("get_window_for_toplevel returned null");
      return;
    }

    MappingRequest request{
        .request = requestHandle,
        .kind = TargetKind::Ext,
        .extToplevel = handle,
    };
    hyprland_toplevel_window_mapping_handle_v1_add_listener(requestHandle, &kMappingListener, this);
    m_pending.emplace(requestHandle, request);
  }

  void HyprlandToplevelMapping::clearRequest(MappingRequest& request) {
    if (request.request != nullptr) {
      hyprland_toplevel_window_mapping_handle_v1_destroy(request.request);
      request.request = nullptr;
    }
    request.wlrToplevel = nullptr;
    request.extToplevel = nullptr;
  }

  void HyprlandToplevelMapping::setWlrWindowId(zwlr_foreign_toplevel_handle_v1* handle, const std::uint64_t address) {
    if (handle == nullptr) {
      return;
    }

    const auto windowId = formatWindowAddress(address);
    if (const auto existing = m_windowIdByWlrHandle.find(handle); existing != m_windowIdByWlrHandle.end()) {
      m_wlrHandleByWindowId.erase(existing->second);
      m_extHandleByWindowId.erase(existing->second);
    }
    m_windowIdByWlrHandle[handle] = windowId;
    m_wlrHandleByWindowId[windowId] = handle;
    notifyChanged();
  }

  void HyprlandToplevelMapping::setExtWindowId(ext_foreign_toplevel_handle_v1* handle, const std::uint64_t address) {
    if (handle == nullptr) {
      return;
    }

    const auto windowId = formatWindowAddress(address);
    if (const auto existing = m_windowIdByExtHandle.find(handle); existing != m_windowIdByExtHandle.end()) {
      m_wlrHandleByWindowId.erase(existing->second);
      m_extHandleByWindowId.erase(existing->second);
    }
    m_windowIdByExtHandle[handle] = windowId;
    m_extHandleByWindowId[windowId] = handle;
    notifyChanged();
  }

  void HyprlandToplevelMapping::notifyChanged() {
    if (m_changeCallback) {
      m_changeCallback();
    }
  }

  void HyprlandToplevelMapping::handleWindowAddress(
      void* data, hyprland_toplevel_window_mapping_handle_v1* requestHandle, const std::uint32_t addressHi,
      const std::uint32_t addressLo
  ) {
    auto* self = static_cast<HyprlandToplevelMapping*>(data);
    if (self == nullptr || requestHandle == nullptr) {
      return;
    }

    const auto it = self->m_pending.find(requestHandle);
    if (it == self->m_pending.end()) {
      return;
    }

    const std::uint64_t address = (static_cast<std::uint64_t>(addressHi) << 32) | static_cast<std::uint64_t>(addressLo);
    MappingRequest request = it->second;
    self->m_pending.erase(it);
    const auto kind = request.kind;
    auto* wlrToplevel = request.wlrToplevel;
    auto* extToplevel = request.extToplevel;
    self->clearRequest(request);

    if (kind == TargetKind::Ext && extToplevel != nullptr) {
      self->setExtWindowId(extToplevel, address);
      return;
    }
    if (kind == TargetKind::Wlr && wlrToplevel != nullptr) {
      self->setWlrWindowId(wlrToplevel, address);
    }
  }

  void HyprlandToplevelMapping::handleFailed(void* data, hyprland_toplevel_window_mapping_handle_v1* requestHandle) {
    auto* self = static_cast<HyprlandToplevelMapping*>(data);
    if (self == nullptr || requestHandle == nullptr) {
      return;
    }

    const auto it = self->m_pending.find(requestHandle);
    if (it == self->m_pending.end()) {
      return;
    }

    self->clearRequest(it->second);
    self->m_pending.erase(it);
  }

} // namespace compositors::hyprland
