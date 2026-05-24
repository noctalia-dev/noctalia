#include "shell/surface_shadow.h"

#include "render/core/color.h"

#include <algorithm>

namespace shell::surface_shadow {

  bool enabled(bool componentShadow, const ShellConfig::ShadowConfig& shadow) noexcept {
    return componentShadow && shadow.blur > 0;
  }

  Bleed bleed(bool componentShadow, const ShellConfig::ShadowConfig& shadow) noexcept {
    if (!enabled(componentShadow, shadow)) {
      return {};
    }
    return {
        .left = shadow.blur + std::max(0, -shadow.offsetX),
        .right = shadow.blur + std::max(0, shadow.offsetX),
        .up = shadow.blur + std::max(0, -shadow.offsetY),
        .down = shadow.blur + std::max(0, shadow.offsetY),
    };
  }

  RoundedRectStyle
  style(const ShellConfig::ShadowConfig& shadow, float backgroundOpacity, const Shape& shape) noexcept {
    const float shadowAlpha = std::clamp(shadow.alpha, 0.0f, 1.0f) * std::clamp(backgroundOpacity, 0.0f, 1.0f);
    return RoundedRectStyle{
        .fill = rgba(0.0f, 0.0f, 0.0f, shadowAlpha),
        .border = Color{},
        .fillMode = FillMode::Solid,
        .corners = shape.corners,
        .logicalInset = shape.logicalInset,
        .radius = shape.radius,
        .softness = static_cast<float>(std::max(0, shadow.blur)),
        .borderWidth = 0.0f,
        .outerShadow = true,
        .shadowCutoutOffsetX = static_cast<float>(shadow.offsetX),
        .shadowCutoutOffsetY = static_cast<float>(shadow.offsetY),
    };
  }

  bool sameSurfaceMetrics(const ShellConfig::ShadowConfig& previous, const ShellConfig::ShadowConfig& next) noexcept {
    return previous.blur == next.blur && previous.offsetX == next.offsetX && previous.offsetY == next.offsetY;
  }

} // namespace shell::surface_shadow
