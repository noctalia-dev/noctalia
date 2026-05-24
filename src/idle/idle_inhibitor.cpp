#include "idle/idle_inhibitor.h"

#include "core/log.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "ipc/ipc_service.h"
#include "wayland/layer_surface.h"
#include "wayland/wayland_connection.h"

#include <wayland-client.h>

namespace {

  constexpr Logger kLog("idle");

} // namespace

IdleInhibitor::IdleInhibitor() = default;

IdleInhibitor::~IdleInhibitor() {
  destroyInhibitor(false);
  m_surface.reset();
}

bool IdleInhibitor::initialize(WaylandConnection& wayland, RenderContext* renderContext) {
  m_wayland = &wayland;
  m_renderContext = renderContext;
  m_manager = m_wayland->idleInhibitManager();

  if (m_manager == nullptr) {
    kLog.info("idle inhibit protocol unavailable");
  }

  return true;
}

void IdleInhibitor::toggle() { setEnabled(!m_enabled); }

void IdleInhibitor::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }

  m_enabled = enabled;
  syncInhibitor(true);
  notifyChanged();
}

void IdleInhibitor::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void IdleInhibitor::ensureSurface() {
  if (m_surface != nullptr || m_wayland == nullptr || m_renderContext == nullptr || m_wayland->outputs().empty()) {
    return;
  }

  const auto& output = m_wayland->outputs().front();
  LayerSurfaceConfig config{
      .nameSpace = "noctalia-idle-inhibitor",
      .layer = LayerShellLayer::Overlay,
      .anchor = LayerShellAnchor::Top | LayerShellAnchor::Left,
      .width = 1,
      .height = 1,
      .exclusiveZone = -1,
      .marginTop = 0,
      .marginRight = 0,
      .marginBottom = 0,
      .marginLeft = 0,
      .keyboard = LayerShellKeyboard::None,
      .defaultWidth = 1,
      .defaultHeight = 1,
  };

  auto surface = std::make_unique<LayerSurface>(*m_wayland, std::move(config));
  surface->setRenderContext(m_renderContext);
  surface->setConfigureCallback([](std::uint32_t /*width*/, std::uint32_t /*height*/) {});

  if (!surface->initialize(output.output)) {
    kLog.warn("failed to initialize idle inhibitor surface");
    return;
  }

  if (wl_region* region = wl_compositor_create_region(m_wayland->compositor()); region != nullptr) {
    wl_surface_set_input_region(surface->wlSurface(), region);
    wl_region_destroy(region);
    wl_surface_commit(surface->wlSurface());
  }

  m_surface = std::move(surface);
}

void IdleInhibitor::syncInhibitor(bool logTransitions) {
  if (m_manager == nullptr) {
    return;
  }

  if (!m_enabled) {
    destroyInhibitor(logTransitions);
    return;
  }

  ensureSurface();
  if (m_surface == nullptr || m_surface->wlSurface() == nullptr || m_inhibitor != nullptr) {
    return;
  }

  m_inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(m_manager, m_surface->wlSurface());
  if (m_inhibitor != nullptr && logTransitions) {
    kLog.info("idle inhibitor enabled");
  }
}

void IdleInhibitor::destroyInhibitor(bool logDisable) {
  if (m_inhibitor != nullptr) {
    zwp_idle_inhibitor_v1_destroy(m_inhibitor);
    m_inhibitor = nullptr;
    if (logDisable) {
      kLog.info("idle inhibitor disabled");
    }
  }
}

void IdleInhibitor::notifyChanged() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void IdleInhibitor::onOutputChange() {
  if (m_manager == nullptr) {
    return;
  }

  destroyInhibitor(false);
  if (m_surface != nullptr) {
    m_surface.reset();
  }

  if (m_enabled) {
    syncInhibitor(false);
  }
}

void IdleInhibitor::registerIpc(IpcService& ipc) {
  ipc.registerHandler(
      "caffeine-enable",
      [this](const std::string&) -> std::string {
        if (!available())
          return "error: caffeine protocol unavailable\n";
        setEnabled(true);
        return "ok\n";
      },
      "caffeine-enable", "Enable caffeine (idle inhibitor)"
  );

  ipc.registerHandler(
      "caffeine-disable",
      [this](const std::string&) -> std::string {
        if (!available())
          return "error: caffeine protocol unavailable\n";
        setEnabled(false);
        return "ok\n";
      },
      "caffeine-disable", "Disable caffeine (idle inhibitor)"
  );

  ipc.registerHandler(
      "caffeine-toggle",
      [this](const std::string&) -> std::string {
        if (!available())
          return "error: caffeine protocol unavailable\n";
        toggle();
        return "ok\n";
      },
      "caffeine-toggle", "Toggle caffeine (idle inhibitor)"
  );
}
