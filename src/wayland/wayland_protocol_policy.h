#pragma once

#include "compositors/compositor_detect.h"

namespace wayland_protocol_policy {

  [[nodiscard]] constexpr bool shouldBindExtForeignToplevelList(compositors::CompositorKind compositor) noexcept {
    return compositor == compositors::CompositorKind::Niri
        || compositor == compositors::CompositorKind::Hyprland
        || compositor == compositors::CompositorKind::Kde
        || compositor == compositors::CompositorKind::Umbriel
        || compositor == compositors::CompositorKind::Labwc
        || compositor == compositors::CompositorKind::Mango
        || compositor == compositors::CompositorKind::Sway;
  }

} // namespace wayland_protocol_policy
