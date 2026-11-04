#include "ui/dialogs/layer_popup_host.h"

#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>

void LayerPopupHostRegistry::registerHost(
    ContextResolver contextResolver, PopupHook beginAttachedPopup, PopupHook endAttachedPopup,
    FallbackResolver fallbackResolver
) {
  m_hosts.push_back(
      Host{
          .contextResolver = std::move(contextResolver),
          .beginAttachedPopup = std::move(beginAttachedPopup),
          .endAttachedPopup = std::move(endAttachedPopup),
          .fallbackResolver = std::move(fallbackResolver),
      }
  );
}

std::optional<LayerPopupParentContext> LayerPopupHostRegistry::contextForSurface(wl_surface* surface) const {
  if (surface == nullptr) {
    return std::nullopt;
  }

  for (const Host& host : m_hosts) {
    if (!host.contextResolver) {
      continue;
    }
    if (auto context = host.contextResolver(surface); context.has_value()) {
      return context;
    }
  }

  return std::nullopt;
}

std::optional<LayerPopupParentContext> LayerPopupHostRegistry::fallbackContext() const {
  for (const Host& host : m_hosts) {
    if (!host.fallbackResolver) {
      continue;
    }
    if (auto context = host.fallbackResolver(); context.has_value()) {
      return context;
    }
  }

  return std::nullopt;
}

std::pair<std::int32_t, std::int32_t> LayerPopupParentContext::centeringOffset(WaylandConnection& wayland) const {
  if (!usedFallback) {
    return {0, 0};
  }

  const auto* out = wayland.findOutputByWl(output);
  if (out == nullptr) {
    return {0, 0};
  }

  const std::int32_t outputW = out->logicalWidth > 0 ? out->logicalWidth : out->width / std::max(1, out->scale);
  const std::int32_t outputH = out->logicalHeight > 0 ? out->logicalHeight : out->height / std::max(1, out->scale);
  return {(outputW - static_cast<std::int32_t>(width)) / 2, (outputH - static_cast<std::int32_t>(height)) / 2};
}

std::optional<LayerPopupParentContext> LayerPopupHostRegistry::resolveForInput(WaylandConnection& wayland) const {
  const auto trySurface = [this](wl_surface* surface) { return contextForSurface(surface); };

  if (wayland.lastInputSource() == WaylandSeat::InputSource::Keyboard) {
    if (auto context = trySurface(wayland.lastKeyboardSurface()); context.has_value()) {
      return context;
    }
    if (auto context = trySurface(wayland.lastPointerSurface()); context.has_value()) {
      return context;
    }
  } else {
    if (auto context = trySurface(wayland.lastPointerSurface()); context.has_value()) {
      return context;
    }
    if (auto context = trySurface(wayland.lastKeyboardSurface()); context.has_value()) {
      return context;
    }
  }

  auto context = fallbackContext();
  if (context.has_value()) {
    context->usedFallback = true;
  }
  return context;
}

void LayerPopupHostRegistry::beginAttachedPopup(wl_surface* surface) const {
  if (surface == nullptr) {
    return;
  }

  for (const Host& host : m_hosts) {
    if (host.beginAttachedPopup) {
      host.beginAttachedPopup(surface);
    }
  }
}

void LayerPopupHostRegistry::endAttachedPopup(wl_surface* surface) const {
  if (surface == nullptr) {
    return;
  }

  for (const Host& host : m_hosts) {
    if (host.endAttachedPopup) {
      host.endAttachedPopup(surface);
    }
  }
}
