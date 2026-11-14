#pragma once

#include "wayland/popup_parent_attach.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

class WaylandConnection;
struct wl_output;
struct wl_surface;
struct xdg_surface;

struct LayerPopupParentContext {
  wl_surface* surface = nullptr;
  // Exactly one of attach and xdgSurface names the popup parent.
  PopupParentAttach attach;
  xdg_surface* xdgSurface = nullptr;
  wl_output* output = nullptr;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  bool usedFallback = false;

  std::pair<std::int32_t, std::int32_t> centeringOffset(WaylandConnection& wayland) const;
};

class LayerPopupHostRegistry {
public:
  using ContextResolver = std::function<std::optional<LayerPopupParentContext>(wl_surface*)>;
  using PopupHook = std::function<void(wl_surface*)>;
  using FallbackResolver = std::function<std::optional<LayerPopupParentContext>()>;

  void registerHost(
      ContextResolver contextResolver, PopupHook beginAttachedPopup = {}, PopupHook endAttachedPopup = {},
      FallbackResolver fallbackResolver = {}
  );

  std::optional<LayerPopupParentContext> contextForSurface(wl_surface* surface) const;
  std::optional<LayerPopupParentContext> fallbackContext() const;
  std::optional<LayerPopupParentContext> resolveForInput(WaylandConnection& wayland) const;

  void beginAttachedPopup(wl_surface* surface) const;
  void endAttachedPopup(wl_surface* surface) const;

private:
  struct Host {
    ContextResolver contextResolver;
    PopupHook beginAttachedPopup;
    PopupHook endAttachedPopup;
    FallbackResolver fallbackResolver;
  };

  std::vector<Host> m_hosts;
};
