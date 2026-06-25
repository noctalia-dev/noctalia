#pragma once

#include "config/config_types.h"
#include "shell/surface/shadow.h"

#include <cstdint>

class Node;
class PopupSurface;
class RectNode;
struct InputRect;
struct PopupSurfaceConfig;

namespace popup_chrome {

  enum class HorizontalAttachment : std::uint8_t {
    Left,
    Center,
    Right,
  };

  enum class VerticalAttachment : std::uint8_t {
    Top,
    Center,
    Bottom,
  };

  struct Attachment {
    HorizontalAttachment horizontal = HorizontalAttachment::Center;
    VerticalAttachment vertical = VerticalAttachment::Top;
  };

  struct Geometry {
    shell::surface_shadow::Bleed bleed{};
    float contentWidth = 1.0f;
    float contentHeight = 1.0f;
    std::uint32_t surfaceWidth = 1;
    std::uint32_t surfaceHeight = 1;

    [[nodiscard]] float contentX() const noexcept { return static_cast<float>(bleed.left); }
    [[nodiscard]] float contentY() const noexcept { return static_cast<float>(bleed.up); }
    [[nodiscard]] float contentRight() const noexcept { return contentX() + contentWidth; }
    [[nodiscard]] float contentBottom() const noexcept { return contentY() + contentHeight; }
    [[nodiscard]] InputRect inputRect() const noexcept;
  };

  [[nodiscard]] Geometry computeGeometry(
      float contentWidth, float contentHeight, const ShellConfig::ShadowConfig& shadow, bool componentShadow = true
  ) noexcept;
  [[nodiscard]] std::int32_t
  adjustedOffsetX(std::int32_t baseOffset, const Geometry& geometry, HorizontalAttachment attachment) noexcept;
  [[nodiscard]] std::int32_t
  adjustedOffsetY(std::int32_t baseOffset, const Geometry& geometry, VerticalAttachment attachment) noexcept;

  void applyToConfig(PopupSurfaceConfig& config, const Geometry& geometry, Attachment attachment) noexcept;
  void setContentInputRegion(PopupSurface& surface, const Geometry& geometry);
  [[nodiscard]] RectNode* addShadow(
      Node& parent, const Geometry& geometry, const ShellConfig::ShadowConfig& shadow, float radius,
      float backgroundOpacity = 1.0f
  );

} // namespace popup_chrome
