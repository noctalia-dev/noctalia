#include "wayland/layer_surface.h"

#include "core/log.h"
#include "util/sys_utils.h"
#include "wayland/wayland_connection.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <stdexcept>
#include <utility>

namespace {

  constexpr Logger kLog("wayland");

  const zwlr_layer_surface_v1_listener kLayerSurfaceListener = {
      .configure = &LayerSurface::handleConfigure,
      .closed = &LayerSurface::handleClosed,
  };

  bool blurTraceEnabled() {
    static const bool enabled = SysUtils::isEnvFlagOn("NOCTALIA_BLUR_TRACE");
    return enabled;
  }

  void traceLayerCommit(const LayerSurface& surface, const LayerSurfaceConfig& config, std::string_view reason) {
    if (!blurTraceEnabled()) {
      return;
    }

    kLog.debug(
        "blur-trace layer-commit reason={} name={} wl={} requested={}x{} default={}x{} anchor={} margins={},{},{},{} "
        "exclusive={} layer={} keyboard={} prewarm_blur={}",
        reason, surface.debugName(), static_cast<const void*>(surface.wlSurface()), config.width, config.height,
        config.defaultWidth, config.defaultHeight, config.anchor, config.marginTop, config.marginRight,
        config.marginBottom, config.marginLeft, config.exclusiveZone, static_cast<std::uint32_t>(config.layer),
        static_cast<std::uint32_t>(config.keyboard), config.prewarmBlur
    );
  }

} // namespace

LayerShellLayer layerShellLayerFromConfig(std::string_view layer) {
  if (layer == "overlay") {
    return LayerShellLayer::Overlay;
  }
  if (layer == "top") {
    return LayerShellLayer::Top;
  }
  kLog.warn("invalid layer-shell layer '{}'; expected top or overlay", layer);
  return LayerShellLayer::Top;
}

LayerSurface::LayerSurface(WaylandConnection& connection, LayerSurfaceConfig config)
    : Surface(connection), m_config(std::move(config)) {
  setDebugName("layer:" + m_config.nameSpace);
}

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

  if (m_config.prewarmBlur) {
    prepareBlurEffect();
  }
  traceLayerCommit(*this, m_config, "initialize");
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
  if (self->m_closedCallback) {
    self->m_closedCallback();
  }
}

void LayerSurface::requestSize(std::uint32_t width, std::uint32_t height) {
  if (m_layerSurface == nullptr || m_surface == nullptr) {
    return;
  }

  std::uint32_t resolvedWidth = width;
  std::uint32_t resolvedHeight = height;
  if (resolvedWidth == 0) {
    const bool stretchWidth =
        (m_config.anchor & LayerShellAnchor::Left) != 0 && (m_config.anchor & LayerShellAnchor::Right) != 0;
    if (stretchWidth) {
      resolvedWidth = 0;
    } else if (m_config.width != 0) {
      resolvedWidth = m_config.width;
    } else if (Surface::width() != 0) {
      resolvedWidth = Surface::width();
    } else {
      resolvedWidth = std::max(m_config.defaultWidth, 1u);
    }
  }
  if (resolvedHeight == 0) {
    const bool stretchHeight =
        (m_config.anchor & LayerShellAnchor::Top) != 0 && (m_config.anchor & LayerShellAnchor::Bottom) != 0;
    if (stretchHeight) {
      resolvedHeight = 0;
    } else if (m_config.height != 0) {
      resolvedHeight = m_config.height;
    } else if (Surface::height() != 0) {
      resolvedHeight = Surface::height();
    } else {
      resolvedHeight = std::max(m_config.defaultHeight, 1u);
    }
  }

  if (resolvedWidth == m_config.width && resolvedHeight == m_config.height) {
    return;
  }

  m_config.width = resolvedWidth;
  m_config.height = resolvedHeight;
  if (resolvedWidth != 0) {
    m_config.defaultWidth = resolvedWidth;
  }
  if (resolvedHeight != 0) {
    m_config.defaultHeight = resolvedHeight;
  }
  zwlr_layer_surface_v1_set_size(m_layerSurface, resolvedWidth, resolvedHeight);
  traceLayerCommit(*this, m_config, "request-size");
  wl_surface_commit(m_surface);
}

void LayerSurface::setLayer(LayerShellLayer layer) {
  if (m_config.layer == layer) {
    return;
  }
  m_config.layer = layer;
  if (m_layerSurface == nullptr || m_surface == nullptr) {
    return;
  }
  zwlr_layer_surface_v1_set_layer(m_layerSurface, static_cast<std::uint32_t>(layer));
  traceLayerCommit(*this, m_config, "set-layer");
  wl_surface_commit(m_surface);
}

void LayerSurface::setMargins(std::int32_t top, std::int32_t right, std::int32_t bottom, std::int32_t left) {
  if (m_config.marginTop == top
      && m_config.marginRight == right
      && m_config.marginBottom == bottom
      && m_config.marginLeft == left) {
    return;
  }
  m_config.marginTop = top;
  m_config.marginRight = right;
  m_config.marginBottom = bottom;
  m_config.marginLeft = left;
  if (m_layerSurface == nullptr || m_surface == nullptr) {
    return;
  }
  zwlr_layer_surface_v1_set_margin(m_layerSurface, top, right, bottom, left);
  traceLayerCommit(*this, m_config, "set-margins");
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
  traceLayerCommit(*this, m_config, "set-exclusive-zone");
  wl_surface_commit(m_surface);
}

void LayerSurface::setClosedCallback(std::function<void()> callback) { m_closedCallback = std::move(callback); }

void LayerSurface::setKeyboardInteractivity(LayerShellKeyboard mode) {
  if (m_config.keyboard == mode) {
    return;
  }
  m_config.keyboard = mode;
  if (m_layerSurface == nullptr || m_surface == nullptr) {
    return;
  }
  zwlr_layer_surface_v1_set_keyboard_interactivity(m_layerSurface, static_cast<std::uint32_t>(mode));
  traceLayerCommit(*this, m_config, "set-keyboard");
  wl_surface_commit(m_surface);
}

void LayerSurface::setClickThrough(bool clickThrough) {
  if (m_clickThrough == clickThrough) {
    return;
  }
  m_clickThrough = clickThrough;
  applyInputRegion();
  if (m_surface != nullptr) {
    traceLayerCommit(*this, m_config, "set-click-through");
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
