#include "wayland/subsurface.h"

#include "core/log.h"
#include "wayland/wayland_connection.h"

#include <stdexcept>
#include <wayland-client-protocol.h>

namespace {

  constexpr Logger kLog("wayland");

} // namespace

Subsurface::Subsurface(WaylandConnection& connection, SubsurfaceConfig config)
    : Surface(connection), m_config(config) {}

Subsurface::~Subsurface() {
  m_connection.unregisterSurface(m_surface);
  if (m_subsurface != nullptr) {
    wl_subsurface_destroy(m_subsurface);
    m_subsurface = nullptr;
  }
}

bool Subsurface::initialize() {
  kLog.warn("subsurface skipped: parent surface is required");
  return false;
}

bool Subsurface::initialize(wl_surface* parentSurface, wl_output* output) {
  if (m_connection.compositor() == nullptr || m_connection.shm() == nullptr || !m_connection.hasSubcompositor()) {
    kLog.warn("subsurface skipped: missing compositor/shm/subcompositor globals");
    return false;
  }
  if (parentSurface == nullptr) {
    kLog.warn("subsurface skipped: parent surface is null");
    return false;
  }
  if (m_config.width == 0 || m_config.height == 0) {
    kLog.warn("subsurface skipped: invalid size {}x{}", m_config.width, m_config.height);
    return false;
  }

  m_parentSurface = parentSurface;
  m_output = output;

  if (!createWlSurface()) {
    return false;
  }

  std::int32_t bufferScale = 1;
  if (const auto* wlOutput = m_connection.findOutputByWl(output); wlOutput != nullptr) {
    bufferScale = wlOutput->scale;
  }
  m_connection.registerSurfaceOutput(m_surface, output);
  setBufferScale(bufferScale);

  m_subsurface = wl_subcompositor_get_subsurface(m_connection.subcompositor(), m_surface, m_parentSurface);
  if (m_subsurface == nullptr) {
    throw std::runtime_error("failed to create subsurface");
  }

  wl_subsurface_set_position(m_subsurface, m_config.x, m_config.y);
  applyStacking();
  if (m_config.desynchronized) {
    wl_subsurface_set_desync(m_subsurface);
  } else {
    wl_subsurface_set_sync(m_subsurface);
  }

  wl_surface_commit(m_parentSurface);

  setRunning(true);
  onConfigure(m_config.width, m_config.height);

  wl_surface_commit(m_parentSurface);
  return true;
}

void Subsurface::setPosition(std::int32_t x, std::int32_t y) {
  if (m_config.x == x && m_config.y == y) {
    return;
  }
  m_config.x = x;
  m_config.y = y;
  if (m_subsurface == nullptr || m_parentSurface == nullptr) {
    return;
  }
  wl_subsurface_set_position(m_subsurface, x, y);
  wl_surface_commit(m_parentSurface);
}

void Subsurface::setStacking(SubsurfaceStacking stacking) {
  if (m_config.stacking == stacking) {
    return;
  }
  m_config.stacking = stacking;
  applyStacking();
  if (m_parentSurface != nullptr) {
    wl_surface_commit(m_parentSurface);
  }
}

void Subsurface::applyStacking() {
  if (m_subsurface == nullptr || m_parentSurface == nullptr) {
    return;
  }

  if (m_config.stacking == SubsurfaceStacking::BelowParent) {
    wl_subsurface_place_below(m_subsurface, m_parentSurface);
  } else {
    wl_subsurface_place_above(m_subsurface, m_parentSurface);
  }
}
