#include "wayland/output_probe.h"

#include "core/log.h"
#include "wayland/layer_surface.h"
#include "wayland/wayland_connection.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <cerrno>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>

namespace {

  constexpr Logger kLog("output-probe");

  int createAnonFd(std::size_t size) {
    int fd = memfd_create("noctalia-output-probe", MFD_CLOEXEC);
    if (fd < 0) {
      return -1;
    }
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
      close(fd);
      return -1;
    }
    return fd;
  }

  const zwlr_layer_surface_v1_listener kLayerSurfaceListener = {
      .configure = &OutputProbe::handleConfigure,
      .closed = &OutputProbe::handleClosed,
  };

  const wl_surface_listener kSurfaceListener = {
      .enter = &OutputProbe::handleEnter,
      .leave = nullptr,
      .preferred_buffer_scale = nullptr,
      .preferred_buffer_transform = nullptr,
  };

} // namespace

OutputProbe::OutputProbe(WaylandConnection& wayland, std::chrono::milliseconds timeout, Callback callback)
    : m_wayland(wayland), m_callback(std::move(callback)) {
  if (m_wayland.compositor() == nullptr || m_wayland.layerShell() == nullptr || m_wayland.shm() == nullptr) {
    kLog.warn("required globals unavailable; reporting no output");
    finish(nullptr);
    return;
  }
  if (!createBuffer()) {
    kLog.warn("failed to create probe buffer; reporting no output");
    finish(nullptr);
    return;
  }

  m_surface = wl_compositor_create_surface(m_wayland.compositor());
  if (m_surface == nullptr) {
    finish(nullptr);
    return;
  }
  wl_surface_add_listener(m_surface, &kSurfaceListener, this);

  // No output argument: the compositor assigns the surface to the output it
  // would choose for any unpinned layer surface (the focused one).
  m_layerSurface = zwlr_layer_shell_v1_get_layer_surface(
      m_wayland.layerShell(), m_surface, nullptr, static_cast<std::uint32_t>(LayerShellLayer::Background),
      "noctalia-output-probe"
  );
  if (m_layerSurface == nullptr) {
    finish(nullptr);
    return;
  }
  zwlr_layer_surface_v1_add_listener(m_layerSurface, &kLayerSurfaceListener, this);
  zwlr_layer_surface_v1_set_size(m_layerSurface, 1, 1);
  zwlr_layer_surface_v1_set_anchor(m_layerSurface, 0);
  zwlr_layer_surface_v1_set_exclusive_zone(m_layerSurface, -1);
  zwlr_layer_surface_v1_set_keyboard_interactivity(
      m_layerSurface, static_cast<std::uint32_t>(LayerShellKeyboard::None)
  );

  // The probe never takes input — an empty region keeps its stray pixel from
  // swallowing a click before it is torn down.
  if (wl_region* emptyRegion = wl_compositor_create_region(m_wayland.compositor()); emptyRegion != nullptr) {
    wl_surface_set_input_region(m_surface, emptyRegion);
    wl_region_destroy(emptyRegion);
  }

  // Initial commit (no buffer) enters the configure round-trip; the buffer is
  // attached on the first configure, which maps the surface and triggers enter.
  wl_surface_commit(m_surface);

  m_timeoutTimer.start(timeout, [this]() { finish(nullptr); });
}

OutputProbe::~OutputProbe() { teardown(); }

bool OutputProbe::createBuffer() {
  constexpr std::int32_t kWidth = 1;
  constexpr std::int32_t kHeight = 1;
  constexpr std::int32_t kStride = kWidth * 4;
  constexpr auto kSize = static_cast<std::size_t>(kStride * kHeight);

  int fd = createAnonFd(kSize);
  if (fd < 0) {
    kLog.warn("failed to create shm fd: {}", std::strerror(errno));
    return false;
  }
  // ftruncate already zero-fills (transparent ARGB8888).

  wl_shm_pool* pool = wl_shm_create_pool(m_wayland.shm(), fd, static_cast<std::int32_t>(kSize));
  close(fd);
  if (pool == nullptr) {
    return false;
  }

  m_buffer = wl_shm_pool_create_buffer(pool, 0, kWidth, kHeight, kStride, WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy(pool);
  return m_buffer != nullptr;
}

void OutputProbe::teardown() {
  if (m_layerSurface != nullptr) {
    zwlr_layer_surface_v1_destroy(m_layerSurface);
    m_layerSurface = nullptr;
  }
  if (m_surface != nullptr) {
    wl_surface_destroy(m_surface);
    m_surface = nullptr;
  }
  if (m_buffer != nullptr) {
    wl_buffer_destroy(m_buffer);
    m_buffer = nullptr;
  }
}

void OutputProbe::finish(wl_output* output) {
  if (m_finished) {
    return;
  }
  m_finished = true;
  m_timeoutTimer.stop();
  teardown();

  // Move the callback to a local: it may destroy this probe (the owner drops
  // the in-flight probe once it has an answer), which would otherwise free the
  // functor mid-call.
  if (m_callback) {
    Callback callback = std::move(m_callback);
    callback(output);
  }
}

void OutputProbe::handleConfigure(
    void* data, zwlr_layer_surface_v1* layerSurface, std::uint32_t serial, std::uint32_t /*width*/,
    std::uint32_t /*height*/
) {
  zwlr_layer_surface_v1_ack_configure(layerSurface, serial);
  auto* probe = static_cast<OutputProbe*>(data);
  if (probe == nullptr || probe->m_surface == nullptr || probe->m_bufferAttached || probe->m_buffer == nullptr) {
    return;
  }
  wl_surface_attach(probe->m_surface, probe->m_buffer, 0, 0);
  wl_surface_set_buffer_scale(probe->m_surface, 1);
  wl_surface_damage_buffer(probe->m_surface, 0, 0, 1, 1);
  probe->m_bufferAttached = true;
  wl_surface_commit(probe->m_surface);
}

void OutputProbe::handleClosed(void* data, zwlr_layer_surface_v1* /*layerSurface*/) {
  auto* probe = static_cast<OutputProbe*>(data);
  if (probe != nullptr) {
    probe->finish(nullptr);
  }
}

void OutputProbe::handleEnter(void* data, wl_surface* /*surface*/, wl_output* output) {
  auto* probe = static_cast<OutputProbe*>(data);
  if (probe != nullptr) {
    probe->finish(output);
  }
}
