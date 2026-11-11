#include "shell/wayland/hyprland/focus_grab_service.h"

#include "core/log.h"
#include "hyprland-focus-grab-v1-client-protocol.h"

#include <algorithm>

namespace {

  constexpr Logger kLog("focus-grab");
  constexpr std::uint32_t kHyprlandFocusGrabManagerVersion = 1;

  const hyprland_focus_grab_v1_listener kFocusGrabListener = {
      .cleared = &FocusGrab::handleCleared,
  };

} // namespace

FocusGrab::FocusGrab(hyprland_focus_grab_v1* grab) : m_grab(grab) {
  if (m_grab != nullptr) {
    hyprland_focus_grab_v1_add_listener(m_grab, &kFocusGrabListener, this);
  }
}

FocusGrab::~FocusGrab() {
  if (m_grab != nullptr) {
    hyprland_focus_grab_v1_destroy(m_grab);
    m_grab = nullptr;
  }
}

void FocusGrab::addSurface(wl_surface* surface) {
  if (m_grab == nullptr || surface == nullptr) {
    return;
  }
  hyprland_focus_grab_v1_add_surface(m_grab, surface);
}

void FocusGrab::removeSurface(wl_surface* surface) {
  if (m_grab == nullptr || surface == nullptr) {
    return;
  }
  hyprland_focus_grab_v1_remove_surface(m_grab, surface);
}

void FocusGrab::commit() {
  if (m_grab == nullptr) {
    return;
  }
  hyprland_focus_grab_v1_commit(m_grab);
}

void FocusGrab::setOnCleared(ClearedCallback callback) { m_onCleared = std::move(callback); }

void FocusGrab::handleCleared(void* data, hyprland_focus_grab_v1* /*grab*/) {
  auto* self = static_cast<FocusGrab*>(data);
  if (self == nullptr || !self->m_onCleared) {
    return;
  }
  self->m_onCleared();
}

FocusGrabService::FocusGrabService(WaylandConnection& /*connection*/) {}

void FocusGrabService::onGlobal(
    wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version
) {
  if (interface == hyprland_focus_grab_manager_v1_interface.name) {
    const auto bindVersion = std::min(version, kHyprlandFocusGrabManagerVersion);
    m_manager = static_cast<hyprland_focus_grab_manager_v1*>(
        wl_registry_bind(registry, name, &hyprland_focus_grab_manager_v1_interface, bindVersion)
    );
  }
}

void FocusGrabService::releaseGlobals() {
  if (m_manager != nullptr) {
    hyprland_focus_grab_manager_v1_destroy(m_manager);
    m_manager = nullptr;
  }
}

std::unique_ptr<FocusGrab> FocusGrabService::createGrab() {
  if (m_manager == nullptr) {
    return nullptr;
  }
  auto* raw = hyprland_focus_grab_manager_v1_create_grab(m_manager);
  if (raw == nullptr) {
    kLog.warn("create_grab returned null");
    return nullptr;
  }
  return std::unique_ptr<FocusGrab>(new FocusGrab(raw));
}
