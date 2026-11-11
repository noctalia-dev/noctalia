#include "wayland/toplevel_surface.h"

#include "core/log.h"
#include "wayland/wayland_connection.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <span>
#include <stdexcept>
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
  *m_alive = false;
  m_connection.unregisterSurface(m_surface);
  destroyRoleObjects();
}

bool ToplevelSurface::initialize(wl_output* output, const ToplevelSurfaceConfig& config) {
  if (m_surface != nullptr) {
    return false;
  }
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
  m_maximized = m_fullscreen = m_activated = false;
  m_pendingMaximized = m_pendingFullscreen = m_pendingActivated = false;

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
  if (!config.title.empty()) {
    xdg_toplevel_set_title(m_toplevel, config.title.c_str());
  }
  if (!config.appId.empty()) {
    xdg_toplevel_set_app_id(m_toplevel, config.appId.c_str());
  }
  if (config.minWidth > 0 || config.minHeight > 0) {
    xdg_toplevel_set_min_size(
        m_toplevel, static_cast<std::int32_t>(config.minWidth), static_cast<std::int32_t>(config.minHeight)
    );
  }

  const auto alive = m_alive;
  setRunning(true);
  if (const auto callback = m_roleCreatedCallback) {
    callback(m_toplevel);
  }
  if (!*alive) {
    return false;
  }
  if (!isRunning()) {
    destroyRoleObjects();
    destroySurface();
    return false;
  }
  wl_surface_commit(m_surface);
  wl_display_flush(m_connection.display());
  const int result = wl_display_roundtrip(m_connection.display());
  if (!*alive) {
    return false;
  }
  if (result < 0 || !isRunning()) {
    setRunning(false);
    if (result < 0) {
      kLog.warn("toplevel: initial roundtrip failed");
    }
    destroyRoleObjects();
    destroySurface();
    return false;
  }

  return true;
}

void ToplevelSurface::setClosedCallback(std::function<void()> callback) { m_closedCallback = std::move(callback); }

void ToplevelSurface::setMinSize(std::uint32_t minWidth, std::uint32_t minHeight) {
  if (m_toplevel != nullptr) {
    xdg_toplevel_set_min_size(m_toplevel, static_cast<std::int32_t>(minWidth), static_cast<std::int32_t>(minHeight));
  }
}

void ToplevelSurface::beginMove(std::uint32_t serial) {
  if (m_toplevel != nullptr && m_connection.seat() != nullptr && serial != 0) {
    xdg_toplevel_move(m_toplevel, m_connection.seat(), serial);
  }
}

void ToplevelSurface::beginResize(std::uint32_t serial, std::uint32_t edges) {
  if (m_toplevel != nullptr && m_connection.seat() != nullptr && serial != 0) {
    xdg_toplevel_resize(m_toplevel, m_connection.seat(), serial, edges);
  }
}

void ToplevelSurface::minimize() {
  if (m_toplevel != nullptr) {
    xdg_toplevel_set_minimized(m_toplevel);
  }
}

void ToplevelSurface::toggleMaximized() {
  if (m_toplevel == nullptr) {
    return;
  }
  if (m_maximized) {
    xdg_toplevel_unset_maximized(m_toplevel);
  } else {
    xdg_toplevel_set_maximized(m_toplevel);
  }
}

void ToplevelSurface::toggleFullscreen() {
  if (m_toplevel == nullptr) {
    return;
  }
  if (m_fullscreen) {
    xdg_toplevel_unset_fullscreen(m_toplevel);
  } else {
    xdg_toplevel_set_fullscreen(m_toplevel, nullptr);
  }
}

void ToplevelSurface::setTitle(const std::string& title) {
  if (m_toplevel != nullptr) {
    xdg_toplevel_set_title(m_toplevel, title.c_str());
  }
}

void ToplevelSurface::showWindowMenu(std::uint32_t serial, std::int32_t x, std::int32_t y) {
  if (m_toplevel != nullptr && m_connection.seat() != nullptr && serial != 0) {
    xdg_toplevel_show_window_menu(m_toplevel, m_connection.seat(), serial, x, y);
  }
}

void ToplevelSurface::close() {
  if (!isRunning()) {
    return;
  }
  setRunning(false);
  if (const auto callback = m_closedCallback) {
    callback();
  }
}

void ToplevelSurface::setRoleLifecycleCallbacks(
    std::function<void(xdg_toplevel*)> created, std::function<void()> destroying
) {
  if (m_toplevel != nullptr) {
    throw std::logic_error("toplevel role hooks must be registered before initialization");
  }
  m_roleCreatedCallback = std::move(created);
  m_roleDestroyingCallback = std::move(destroying);
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
  self->m_maximized = self->m_pendingMaximized;
  self->m_fullscreen = self->m_pendingFullscreen;
  self->m_activated = self->m_pendingActivated;
  self->Surface::onConfigure(width, height);
}

void ToplevelSurface::handleToplevelConfigure(
    void* data, xdg_toplevel* /*toplevel*/, std::int32_t width, std::int32_t height, wl_array* states
) {
  auto* self = static_cast<ToplevelSurface*>(data);
  self->m_lastToplevelWidth = width;
  self->m_lastToplevelHeight = height;
  const auto values = std::span(static_cast<const std::uint32_t*>(states->data), states->size / sizeof(std::uint32_t));
  self->m_pendingMaximized = std::ranges::contains(values, XDG_TOPLEVEL_STATE_MAXIMIZED);
  self->m_pendingFullscreen = std::ranges::contains(values, XDG_TOPLEVEL_STATE_FULLSCREEN);
  self->m_pendingActivated = std::ranges::contains(values, XDG_TOPLEVEL_STATE_ACTIVATED);
  if (width > 0) {
    self->m_pendingWidth = static_cast<std::uint32_t>(width);
  }
  if (height > 0) {
    self->m_pendingHeight = static_cast<std::uint32_t>(height);
  }
}

void ToplevelSurface::handleToplevelClose(void* data, xdg_toplevel* /*toplevel*/) {
  auto* self = static_cast<ToplevelSurface*>(data);
  self->close();
}

void ToplevelSurface::handleToplevelConfigureBounds(
    void* /*data*/, xdg_toplevel* /*toplevel*/, std::int32_t /*width*/, std::int32_t /*height*/
) {}

void ToplevelSurface::
    handleToplevelWmCapabilities(void* /*data*/, xdg_toplevel* /*toplevel*/, wl_array* /*capabilities*/) {}

void ToplevelSurface::destroyRoleObjects() {
  if (m_toplevel != nullptr) {
    if (const auto callback = m_roleDestroyingCallback) {
      callback();
    }
    xdg_toplevel_destroy(m_toplevel);
    m_toplevel = nullptr;
  }
  if (m_xdgSurface != nullptr) {
    xdg_surface_destroy(m_xdgSurface);
    m_xdgSurface = nullptr;
  }
}
