#pragma once

#include "wayland/layer_surface.h"
#include "wayland/surface.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

class WaylandConnection;
struct PointerEvent;
struct wl_buffer;
struct wl_output;
struct wl_surface;
struct wp_viewport;
struct zwlr_layer_surface_v1;

// Transparent fullscreen layer-shell surfaces (one per output) that catch
// clicks landing outside the active panel and outside the bars. Used to
// dismiss panels when the user clicks on an xdg_toplevel app window — Wayland
// routes that click to the app's surface, so without a catcher the panel
// never sees it.
//
// Ordering: shields are mapped on the same layer as the panel; activate()
// must be called BEFORE the panel surface is committed so that the panel
// ends up on top of its co-output shield within the layer (wlroots stacks
// within-layer surfaces in mapping order).
//
// Each shield's input region is fullscreen MINUS the rects returned by the
// exclude provider for that output, so clicks on bar widgets keep flowing
// to the bar.
//
// Keyboard interactivity:
//   - Hyprland gates pointer delivery on keyboard_interactivity: layer-shell
//     surfaces declared as None never receive pointer events, so the shield
//     uses Exclusive there. The panel is also Exclusive and is mapped after
//     the shield, so per the layer-shell spec the panel still wins keyboard
//     focus.
//   - On every other compositor we tested (niri, wlroots vanilla, sway), None
//     works fine and avoids touching keyboard focus at all, so we keep that
//     as the default.
//
// Buffer strategy: a single shared 1×1 fully-transparent SHM buffer is
// stretched to the per-shield surface size via wp_viewport. Cost is ~4 bytes
// regardless of resolution or output count.
class PanelClickShield {
public:
  using ExcludeProvider = std::function<std::vector<InputRect>(wl_output*)>;

  PanelClickShield() = default;
  ~PanelClickShield();

  PanelClickShield(const PanelClickShield&) = delete;
  PanelClickShield& operator=(const PanelClickShield&) = delete;

  void initialize(WaylandConnection& wayland);

  // Map a fullscreen shield on each of the given outputs.
  void activate(const std::vector<wl_output*>& outputs, LayerShellLayer layer, ExcludeProvider excludeProvider);

  // Tear down all shields. Idempotent.
  void deactivate();

  [[nodiscard]] bool isActive() const noexcept { return !m_shields.empty(); }
  [[nodiscard]] bool ownsSurface(wl_surface* surface) const noexcept;

  // Public so the C-callback bridge in panel_click_shield.cpp can dispatch.
  static void handleConfigure(
      void* data, zwlr_layer_surface_v1* layerSurface, std::uint32_t serial, std::uint32_t width, std::uint32_t height
  );
  static void handleClosed(void* data, zwlr_layer_surface_v1* layerSurface);

private:
  struct Shield {
    PanelClickShield* owner = nullptr;
    wl_output* output = nullptr;
    wl_surface* surface = nullptr;
    zwlr_layer_surface_v1* layerSurface = nullptr;
    wp_viewport* viewport = nullptr;
    std::int32_t width = 0;
    std::int32_t height = 0;
    bool configured = false;
    bool bufferAttached = false;
    std::vector<InputRect> excludeRects;
  };

  bool ensureSharedBuffer();
  std::unique_ptr<Shield> createShield(wl_output* output, LayerShellLayer layer, std::vector<InputRect> excludeRects);
  void destroyShield(Shield& shield);
  void applyConfigured(Shield& shield, std::uint32_t width, std::uint32_t height);
  void applyInputRegion(Shield& shield);

  WaylandConnection* m_wayland = nullptr;
  wl_buffer* m_buffer = nullptr;
  std::unordered_map<wl_output*, std::unique_ptr<Shield>> m_shields;
};
