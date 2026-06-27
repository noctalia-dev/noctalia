#include "wayland/toplevel_surface.h"

#include "core/log.h"
#include "wayland/wayland_connection.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <utility>

namespace {

  constexpr Logger kLog("wayland");

  const xdg_surface_listener kXdgSurfaceListener = {
      .configure = &ToplevelSurface::handleXdgSurfaceConfigure,
  };

  const xdg_toplevel_listener kToplevelListener = {
      .configure = &ToplevelSurface::handleToplevelConfigure,
      .close = &ToplevelSurface::handleToplevelClose,
      .configure_bounds = &ToplevelSurface::handleToplevelConfigureBounds,
      .wm_capabilities = &ToplevelSurface::handleToplevelWmCapabilities,
  };

} // namespace

ToplevelSurface::ToplevelSurface(WaylandConnection& connection) : Surface(connection) {}

ToplevelSurface::~ToplevelSurface() {
  m_connection.unregisterSurface(m_surface);
  destroyRoleObjects();
}

bool ToplevelSurface::initialize(wl_output* output, ToplevelSurfaceConfig config) {
  if (!m_connection.hasXdgShell()) {
    kLog.warn("toplevel: missing xdg-shell");
    return false;
  }

  if (!createWlSurface()) {
    return false;
  }

  std::int32_t bufferScale = 1;
  if (output != nullptr) {
    if (const auto* wlOutput = m_connection.findOutputByWl(output); wlOutput != nullptr) {
      bufferScale = wlOutput->scale;
    }
    m_connection.registerSurfaceOutput(m_surface, output);
  }
  setBufferScale(bufferScale);

  m_pendingWidth = std::max<std::uint32_t>(1, config.width);
  m_pendingHeight = std::max<std::uint32_t>(1, config.height);
  m_lastToplevelWidth = 0;
  m_lastToplevelHeight = 0;

  m_xdgSurface = xdg_wm_base_get_xdg_surface(m_connection.xdgWmBase(), m_surface);
  if (m_xdgSurface == nullptr) {
    destroySurface();
    return false;
  }
  xdg_surface_add_listener(m_xdgSurface, &kXdgSurfaceListener, this);

  m_toplevel = xdg_surface_get_toplevel(m_xdgSurface);
  if (m_toplevel == nullptr) {
    destroyRoleObjects();
    destroySurface();
    return false;
  }

  xdg_toplevel_add_listener(m_toplevel, &kToplevelListener, this);
  xdg_toplevel_set_title(m_toplevel, config.title.empty() ? "Noctalia" : config.title.c_str());
  xdg_toplevel_set_app_id(m_toplevel, config.appId != nullptr ? config.appId : "dev.noctalia.Noctalia");
  if (config.minWidth > 0 || config.minHeight > 0) {
    xdg_toplevel_set_min_size(
        m_toplevel, static_cast<std::int32_t>(config.minWidth), static_cast<std::int32_t>(config.minHeight)
    );
  }

  wl_surface_commit(m_surface);
  wl_display_flush(m_connection.display());
  if (wl_display_roundtrip(m_connection.display()) < 0) {
    kLog.warn("toplevel: initial roundtrip failed");
    destroyRoleObjects();
    destroySurface();
    return false;
  }

  setRunning(true);
  return true;
}

void ToplevelSurface::setClosedCallback(std::function<void()> callback) { m_closedCallback = std::move(callback); }

void ToplevelSurface::setMinSize(std::uint32_t minWidth, std::uint32_t minHeight) {
  if (m_toplevel != nullptr) {
    xdg_toplevel_set_min_size(m_toplevel, static_cast<std::int32_t>(minWidth), static_cast<std::int32_t>(minHeight));
  }
}

void ToplevelSurface::clampToMinSize(std::uint32_t minWidth, std::uint32_t minHeight) {
  const auto w = std::max(width(), minWidth);
  const auto h = std::max(height(), minHeight);
  if (w != width() || h != height()) {
    onConfigure(w, h);
  }
}

void ToplevelSurface::beginMove(std::uint32_t serial) {
  if (m_toplevel != nullptr) {
    xdg_toplevel_move(m_toplevel, m_connection.seat(), serial);
  }
}

void ToplevelSurface::handleXdgSurfaceConfigure(void* data, xdg_surface* surface, std::uint32_t serial) {
  auto* self = static_cast<ToplevelSurface*>(data);
  xdg_surface_ack_configure(surface, serial);

  std::uint32_t width = self->m_pendingWidth;
  std::uint32_t height = self->m_pendingHeight;
  if (self->m_lastToplevelWidth > 0) {
    width = static_cast<std::uint32_t>(self->m_lastToplevelWidth);
  }
  if (self->m_lastToplevelHeight > 0) {
    height = static_cast<std::uint32_t>(self->m_lastToplevelHeight);
  }
  width = std::max<std::uint32_t>(1, width);
  height = std::max<std::uint32_t>(1, height);
  self->Surface::onConfigure(width, height);
}

void ToplevelSurface::handleToplevelConfigure(
    void* data, xdg_toplevel* /*toplevel*/, std::int32_t width, std::int32_t height, wl_array* /*states*/
) {
  auto* self = static_cast<ToplevelSurface*>(data);
  self->m_lastToplevelWidth = width;
  self->m_lastToplevelHeight = height;
  if (width > 0) {
    self->m_pendingWidth = static_cast<std::uint32_t>(width);
  }
  if (height > 0) {
    self->m_pendingHeight = static_cast<std::uint32_t>(height);
  }
}

void ToplevelSurface::handleToplevelClose(void* data, xdg_toplevel* /*toplevel*/) {
  auto* self = static_cast<ToplevelSurface*>(data);
  self->setRunning(false);
  if (self->m_closedCallback) {
    self->m_closedCallback();
  }
}

void ToplevelSurface::handleToplevelConfigureBounds(
    void* /*data*/, xdg_toplevel* /*toplevel*/, std::int32_t /*width*/, std::int32_t /*height*/
) {}

void ToplevelSurface::
    handleToplevelWmCapabilities(void* /*data*/, xdg_toplevel* /*toplevel*/, wl_array* /*capabilities*/) {}

void ToplevelSurface::destroyRoleObjects() {
  if (m_toplevel != nullptr) {
    xdg_toplevel_destroy(m_toplevel);
    m_toplevel = nullptr;
  }
  if (m_xdgSurface != nullptr) {
    xdg_surface_destroy(m_xdgSurface);
    m_xdgSurface = nullptr;
  }
}
