#include "wayland/layer_surface.h"

#include "core/log.h"
#include "wayland/wayland_connection.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <stdexcept>
#include <wayland-client.h>

namespace {

  constexpr Logger kLog("wayland");

  const zwlr_layer_surface_v1_listener kLayerSurfaceListener = {
      .configure = &LayerSurface::handleConfigure,
      .closed = &LayerSurface::handleClosed,
  };

} // namespace

LayerSurface::LayerSurface(WaylandConnection& connection, LayerSurfaceConfig config)
    : Surface(connection), m_config(std::move(config)) {}

LayerSurface::~LayerSurface() {
  m_connection.unregisterSurface(m_surface);
  if (m_layerSurface != nullptr) {
    zwlr_layer_surface_v1_destroy(m_layerSurface);
    m_layerSurface = nullptr;
  }
}

bool LayerSurface::initialize() {
  wl_output* output = nullptr;
  if (!m_connection.outputs().empty()) {
    output = m_connection.outputs().front().output;
  }
  return initialize(output);
}

bool LayerSurface::initialize(wl_output* output) {
  if (!m_connection.hasRequiredGlobals()) {
    kLog.warn("layer surface skipped: missing compositor/shm/layer-shell globals");
    return false;
  }

  if (!createWlSurface()) {
    return false;
  }

  std::int32_t bufferScale = 1;
  if (const auto* wlOutput = m_connection.findOutputByWl(output); wlOutput != nullptr) {
    bufferScale = wlOutput->scale;
  }

  m_connection.registerSurfaceOutput(m_surface, output);
  setBufferScale(bufferScale);

  m_layerSurface = zwlr_layer_shell_v1_get_layer_surface(
      m_connection.layerShell(), m_surface, output, static_cast<std::uint32_t>(m_config.layer),
      m_config.nameSpace.c_str()
  );
  if (m_layerSurface == nullptr) {
    throw std::runtime_error("failed to create layer surface");
  }

  if (zwlr_layer_surface_v1_add_listener(m_layerSurface, &kLayerSurfaceListener, this) != 0) {
    throw std::runtime_error("failed to add layer surface listener");
  }

  m_connection.registerLayerSurface(m_surface, m_layerSurface);

  zwlr_layer_surface_v1_set_anchor(m_layerSurface, m_config.anchor);
  zwlr_layer_surface_v1_set_size(m_layerSurface, m_config.width, m_config.height);
  zwlr_layer_surface_v1_set_exclusive_zone(m_layerSurface, m_config.exclusiveZone);
  zwlr_layer_surface_v1_set_margin(
      m_layerSurface, m_config.marginTop, m_config.marginRight, m_config.marginBottom, m_config.marginLeft
  );
  zwlr_layer_surface_v1_set_keyboard_interactivity(m_layerSurface, static_cast<std::uint32_t>(m_config.keyboard));
  applyInputRegion();

  wl_surface_commit(m_surface);

  setRunning(true);
  return true;
}

void LayerSurface::handleConfigure(
    void* data, zwlr_layer_surface_v1* layerSurface, std::uint32_t serial, std::uint32_t width, std::uint32_t height
) {
  auto* self = static_cast<LayerSurface*>(data);
  zwlr_layer_surface_v1_ack_configure(layerSurface, serial);

  self->onConfigure(
      (width == 0) ? self->m_config.defaultWidth : width, (height == 0) ? self->m_config.defaultHeight : height
  );
}

void LayerSurface::handleClosed(void* data, zwlr_layer_surface_v1* /*layerSurface*/) {
  auto* self = static_cast<LayerSurface*>(data);
  self->setRunning(false);
}

void LayerSurface::requestSize(std::uint32_t width, std::uint32_t height) {
  if (m_layerSurface == nullptr || m_surface == nullptr) {
    return;
  }
  m_config.width = width;
  m_config.height = height;
  m_config.defaultWidth = width;
  m_config.defaultHeight = height;
  zwlr_layer_surface_v1_set_size(m_layerSurface, width, height);
  wl_surface_commit(m_surface);
}

void LayerSurface::setMargins(std::int32_t top, std::int32_t right, std::int32_t bottom, std::int32_t left) {
  m_config.marginTop = top;
  m_config.marginRight = right;
  m_config.marginBottom = bottom;
  m_config.marginLeft = left;
  if (m_layerSurface == nullptr || m_surface == nullptr) {
    return;
  }
  zwlr_layer_surface_v1_set_margin(m_layerSurface, top, right, bottom, left);
  wl_surface_commit(m_surface);
}

void LayerSurface::setExclusiveZone(std::int32_t exclusiveZone) {
  if (m_config.exclusiveZone == exclusiveZone) {
    return;
  }
  m_config.exclusiveZone = exclusiveZone;
  if (m_layerSurface == nullptr || m_surface == nullptr) {
    return;
  }
  zwlr_layer_surface_v1_set_exclusive_zone(m_layerSurface, exclusiveZone);
  wl_surface_commit(m_surface);
}

void LayerSurface::setKeyboardInteractivity(LayerShellKeyboard mode) {
  if (m_config.keyboard == mode) {
    return;
  }
  m_config.keyboard = mode;
  if (m_layerSurface == nullptr || m_surface == nullptr) {
    return;
  }
  zwlr_layer_surface_v1_set_keyboard_interactivity(m_layerSurface, static_cast<std::uint32_t>(mode));
  wl_surface_commit(m_surface);
}

void LayerSurface::setClickThrough(bool clickThrough) {
  if (m_clickThrough == clickThrough) {
    return;
  }
  m_clickThrough = clickThrough;
  applyInputRegion();
  if (m_surface != nullptr) {
    wl_surface_commit(m_surface);
  }
}

void LayerSurface::applyInputRegion() {
  if (m_surface == nullptr) {
    return;
  }

  if (m_clickThrough) {
    setInputRegion({});
    return;
  }

  wl_surface_set_input_region(m_surface, nullptr);
}
