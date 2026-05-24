#include "shell/panel/panel_click_shield.h"

#include "compositors/compositor_detect.h"
#include "core/log.h"
#include "viewporter-client-protocol.h"
#include "wayland/wayland_connection.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

namespace {

  constexpr Logger kLog("panel-click-shield");

  // Anonymous file backing for a tiny SHM pool. We use memfd_create so the fd
  // is never visible on the filesystem.
  int createAnonFd(std::size_t size) {
    int fd = memfd_create("noctalia-click-shield", MFD_CLOEXEC);
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
      .configure = &PanelClickShield::handleConfigure,
      .closed = &PanelClickShield::handleClosed,
  };

  // Hyprland refuses to deliver pointer events to layer-shell surfaces with
  // keyboard_interactivity == None. Exclusive is what unlocks pointer delivery
  // there. Bar exclusion via input region doesn't actually work on Hyprland
  // (clicks on bars still hit the shield); on Hyprland we prefer the
  // hyprland_focus_grab_v1 protocol when available and only fall back to the
  // shield path here.
  LayerShellKeyboard shieldKeyboardMode() {
    return compositors::isHyprland() ? LayerShellKeyboard::Exclusive : LayerShellKeyboard::None;
  }

} // namespace

PanelClickShield::~PanelClickShield() {
  deactivate();
  if (m_buffer != nullptr) {
    wl_buffer_destroy(m_buffer);
    m_buffer = nullptr;
  }
}

void PanelClickShield::initialize(WaylandConnection& wayland) { m_wayland = &wayland; }

bool PanelClickShield::ensureSharedBuffer() {
  if (m_buffer != nullptr) {
    return true;
  }
  if (m_wayland == nullptr || m_wayland->shm() == nullptr) {
    return false;
  }

  // 1×1 ARGB8888 — 4 bytes total. Stretched to fullscreen via wp_viewport.
  constexpr std::int32_t kWidth = 1;
  constexpr std::int32_t kHeight = 1;
  constexpr std::int32_t kStride = kWidth * 4;
  constexpr std::size_t kSize = static_cast<std::size_t>(kStride * kHeight);

  int fd = createAnonFd(kSize);
  if (fd < 0) {
    kLog.warn("failed to create shm fd: {}", std::strerror(errno));
    return false;
  }
  // ftruncate already zero-fills (transparent ARGB8888).

  wl_shm_pool* pool = wl_shm_create_pool(m_wayland->shm(), fd, static_cast<std::int32_t>(kSize));
  close(fd);
  if (pool == nullptr) {
    return false;
  }

  m_buffer = wl_shm_pool_create_buffer(pool, 0, kWidth, kHeight, kStride, WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy(pool);

  return m_buffer != nullptr;
}

void PanelClickShield::activate(
    const std::vector<wl_output*>& outputs, LayerShellLayer layer, ExcludeProvider excludeProvider
) {
  if (m_wayland == nullptr) {
    return;
  }

  // Tear down any previous shields and recreate. Cheaper to refresh than to
  // diff outputs/exclude rects, and keeps the dispatch order deterministic.
  deactivate();

  if (!ensureSharedBuffer()) {
    kLog.warn("disabled: shared shm buffer unavailable");
    return;
  }
  if (m_wayland->layerShell() == nullptr || m_wayland->compositor() == nullptr) {
    return;
  }
  if (m_wayland->viewporter() == nullptr) {
    // Without viewporter we'd have to allocate a fullscreen-sized buffer to
    // make the surface logically fullscreen. Skip the shield rather than burn
    // tens of MB of SHM. Most modern compositors support viewporter.
    kLog.warn("disabled: wp_viewporter not available");
    return;
  }

  for (wl_output* output : outputs) {
    if (output == nullptr || m_shields.find(output) != m_shields.end()) {
      continue;
    }
    std::vector<InputRect> excludeRects;
    if (excludeProvider) {
      excludeRects = excludeProvider(output);
    }
    auto shield = createShield(output, layer, std::move(excludeRects));
    if (shield) {
      m_shields.emplace(output, std::move(shield));
    }
  }
}

std::unique_ptr<PanelClickShield::Shield>
PanelClickShield::createShield(wl_output* output, LayerShellLayer layer, std::vector<InputRect> excludeRects) {
  auto shield = std::make_unique<Shield>();
  shield->owner = this;
  shield->output = output;
  shield->excludeRects = std::move(excludeRects);

  shield->surface = wl_compositor_create_surface(m_wayland->compositor());
  if (shield->surface == nullptr) {
    return nullptr;
  }

  shield->viewport = wp_viewporter_get_viewport(m_wayland->viewporter(), shield->surface);

  shield->layerSurface = zwlr_layer_shell_v1_get_layer_surface(
      m_wayland->layerShell(), shield->surface, output, static_cast<std::uint32_t>(layer), "noctalia-panel-click-shield"
  );
  if (shield->layerSurface == nullptr) {
    if (shield->viewport != nullptr) {
      wp_viewport_destroy(shield->viewport);
    }
    wl_surface_destroy(shield->surface);
    return nullptr;
  }

  zwlr_layer_surface_v1_add_listener(shield->layerSurface, &kLayerSurfaceListener, shield.get());

  zwlr_layer_surface_v1_set_anchor(
      shield->layerSurface,
      LayerShellAnchor::Top | LayerShellAnchor::Bottom | LayerShellAnchor::Left | LayerShellAnchor::Right
  );
  zwlr_layer_surface_v1_set_size(shield->layerSurface, 0, 0);
  zwlr_layer_surface_v1_set_exclusive_zone(shield->layerSurface, -1);
  zwlr_layer_surface_v1_set_keyboard_interactivity(
      shield->layerSurface, static_cast<std::uint32_t>(shieldKeyboardMode())
  );

  // Empty input region until we receive a configure with the actual surface
  // size. Until then any click would land on the 1×1 buffer at (0,0) before
  // the viewport applies, which we don't want.
  if (wl_region* emptyRegion = wl_compositor_create_region(m_wayland->compositor()); emptyRegion != nullptr) {
    wl_surface_set_input_region(shield->surface, emptyRegion);
    wl_region_destroy(emptyRegion);
  }

  // Initial commit (no buffer) — required by layer-shell to enter the
  // configure round-trip. The buffer is attached on the first configure.
  wl_surface_commit(shield->surface);

  return shield;
}

void PanelClickShield::deactivate() {
  for (auto& [output, shield] : m_shields) {
    if (shield) {
      destroyShield(*shield);
    }
  }
  m_shields.clear();
}

void PanelClickShield::destroyShield(Shield& shield) {
  if (shield.viewport != nullptr) {
    wp_viewport_destroy(shield.viewport);
    shield.viewport = nullptr;
  }
  if (shield.layerSurface != nullptr) {
    zwlr_layer_surface_v1_destroy(shield.layerSurface);
    shield.layerSurface = nullptr;
  }
  if (shield.surface != nullptr) {
    wl_surface_destroy(shield.surface);
    shield.surface = nullptr;
  }
}

bool PanelClickShield::ownsSurface(wl_surface* surface) const noexcept {
  if (surface == nullptr) {
    return false;
  }
  for (const auto& [output, shield] : m_shields) {
    if (shield && shield->surface == surface) {
      return true;
    }
  }
  return false;
}

void PanelClickShield::handleConfigure(
    void* data, zwlr_layer_surface_v1* layerSurface, std::uint32_t serial, std::uint32_t width, std::uint32_t height
) {
  zwlr_layer_surface_v1_ack_configure(layerSurface, serial);
  auto* shield = static_cast<Shield*>(data);
  if (shield == nullptr || shield->owner == nullptr) {
    return;
  }
  shield->owner->applyConfigured(*shield, width, height);
}

void PanelClickShield::handleClosed(void* data, zwlr_layer_surface_v1* /*layerSurface*/) {
  auto* shield = static_cast<Shield*>(data);
  if (shield == nullptr || shield->owner == nullptr) {
    return;
  }
  // Compositor closed our surface. Drop just this shield; the others (and
  // the next activate()) keep working.
  PanelClickShield* owner = shield->owner;
  wl_output* output = shield->output;
  auto it = owner->m_shields.find(output);
  if (it != owner->m_shields.end() && it->second.get() == shield) {
    owner->destroyShield(*it->second);
    owner->m_shields.erase(it);
  }
}

void PanelClickShield::applyConfigured(Shield& shield, std::uint32_t width, std::uint32_t height) {
  if (shield.surface == nullptr) {
    return;
  }

  shield.width = static_cast<std::int32_t>(width);
  shield.height = static_cast<std::int32_t>(height);

  if (!shield.bufferAttached && m_buffer != nullptr) {
    wl_surface_attach(shield.surface, m_buffer, 0, 0);
    wl_surface_set_buffer_scale(shield.surface, 1);
    wl_surface_damage_buffer(shield.surface, 0, 0, 1, 1);
    shield.bufferAttached = true;
  }

  if (shield.viewport != nullptr && width > 0 && height > 0) {
    wp_viewport_set_destination(shield.viewport, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height));
  }

  shield.configured = true;
  applyInputRegion(shield);
  wl_surface_commit(shield.surface);
}

void PanelClickShield::applyInputRegion(Shield& shield) {
  if (m_wayland == nullptr || shield.surface == nullptr) {
    return;
  }
  if (shield.width <= 0 || shield.height <= 0) {
    return;
  }

  wl_region* region = wl_compositor_create_region(m_wayland->compositor());
  if (region == nullptr) {
    return;
  }

  wl_region_add(region, 0, 0, shield.width, shield.height);
  for (const auto& r : shield.excludeRects) {
    wl_region_subtract(region, r.x, r.y, r.width, r.height);
  }

  wl_surface_set_input_region(shield.surface, region);
  wl_region_destroy(region);
}
