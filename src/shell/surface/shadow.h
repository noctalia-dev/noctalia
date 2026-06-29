#pragma once

#include "config/config_types.h"
#include "render/core/render_styles.h"

#include <cstdint>

namespace shell::surface_shadow {

  constexpr std::int32_t kBlurRadius = 12;

  struct Bleed {
    std::int32_t left = 0;
    std::int32_t right = 0;
    std::int32_t up = 0;
    std::int32_t down = 0;
  };

  struct Shape {
    CornerShapes corners{};
    RectInsets logicalInset{};
    Radii radius;
  };

  [[nodiscard]] bool enabled(bool componentShadow, const ShellConfig::ShadowConfig& shadow) noexcept;
  [[nodiscard]] Bleed bleed(bool componentShadow, const ShellConfig::ShadowConfig& shadow) noexcept;
  [[nodiscard]] RoundedRectStyle
  style(const ShellConfig::ShadowConfig& shadow, float backgroundOpacity, const Shape& shape) noexcept;
  [[nodiscard]] bool
  sameSurfaceMetrics(const ShellConfig::ShadowConfig& previous, const ShellConfig::ShadowConfig& next) noexcept;

} // namespace shell::surface_shadow
