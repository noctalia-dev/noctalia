#pragma once

#include "compositors/compositor_detect.h"

namespace wayland_protocol_policy {

  [[nodiscard]] constexpr bool shouldBindExtForeignToplevelList(compositors::CompositorKind compositor) noexcept {
    return compositor == compositors::CompositorKind::Niri
        || compositor == compositors::CompositorKind::Hyprland
        || compositor == compositors::CompositorKind::Kde;
  }

} // namespace wayland_protocol_policy
