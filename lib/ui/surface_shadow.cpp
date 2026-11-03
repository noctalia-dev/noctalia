#include "ui/surface_shadow.h"

#include "render/core/color.h"

#include <algorithm>

namespace surface_shadow {

  bool enabled(bool componentShadow, const ShadowStyle& /*shadow*/) noexcept { return componentShadow; }

  Bleed bleed(bool componentShadow, const ShadowStyle& shadow) noexcept {
    if (!enabled(componentShadow, shadow)) {
      return {};
    }
    const auto offset = shadowDirectionOffset(shadow.direction);
    return {
        .left = kBlurRadius + std::max(0, -offset.x),
        .right = kBlurRadius + std::max(0, offset.x),
        .up = kBlurRadius + std::max(0, -offset.y),
        .down = kBlurRadius + std::max(0, offset.y),
    };
  }

  RoundedRectStyle style(const ShadowStyle& shadow, float backgroundOpacity, const Shape& shape) noexcept {
    const auto offset = shadowDirectionOffset(shadow.direction);
    const float shadowAlpha = std::clamp(shadow.alpha, 0.0F, 1.0F) * std::clamp(backgroundOpacity, 0.0F, 1.0F);
    return RoundedRectStyle{
        .fill = rgba(0.0F, 0.0F, 0.0F, shadowAlpha),
        .border = Color{},
        .fillMode = FillMode::Solid,
        .corners = shape.corners,
        .logicalInset = shape.logicalInset,
        .radius = shape.radius,
        .softness = static_cast<float>(kBlurRadius),
        .borderWidth = 0.0F,
        .outerShadow = true,
        .shadowCutoutOffsetX = static_cast<float>(offset.x),
        .shadowCutoutOffsetY = static_cast<float>(offset.y),
    };
  }

  MaskStyle maskStyle(const ShadowStyle& shadow) noexcept {
    return {.tint = style(shadow, 1.0F, {}).fill, .offset = shadowDirectionOffset(shadow.direction)};
  }

  bool sameSurfaceMetrics(const ShadowStyle& previous, const ShadowStyle& next) noexcept {
    return previous.direction == next.direction;
  }

} // namespace surface_shadow
