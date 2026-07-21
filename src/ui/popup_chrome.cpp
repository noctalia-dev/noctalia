#include "ui/popup_chrome.h"

#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "ui/controls/box.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

  constexpr std::int32_t kShadowSafetyPadding = 2;

  std::uint32_t extentFor(float value) noexcept { return static_cast<std::uint32_t>(std::max(1.0f, std::ceil(value))); }

  int roundedInt(float value) noexcept { return static_cast<int>(std::lround(value)); }

} // namespace

namespace popup_chrome {

  InputRect Geometry::inputRect() const noexcept {
    return InputRect{
        .x = bleed.left,
        .y = bleed.up,
        .width = roundedInt(contentWidth),
        .height = roundedInt(contentHeight),
    };
  }

  Geometry computeGeometry(
      float contentWidth, float contentHeight, const ShellConfig::ShadowConfig& shadow, bool componentShadow
  ) noexcept {
    Geometry geometry{
        .contentWidth = std::max(1.0f, contentWidth),
        .contentHeight = std::max(1.0f, contentHeight),
    };

    if (shell::surface_shadow::enabled(componentShadow, shadow)) {
      geometry.bleed = shell::surface_shadow::bleed(componentShadow, shadow);
      geometry.bleed.left += kShadowSafetyPadding;
      geometry.bleed.right += kShadowSafetyPadding;
      geometry.bleed.up += kShadowSafetyPadding;
      geometry.bleed.down += kShadowSafetyPadding;
    }

    geometry.surfaceWidth =
        extentFor(geometry.contentWidth + static_cast<float>(geometry.bleed.left + geometry.bleed.right));
    geometry.surfaceHeight =
        extentFor(geometry.contentHeight + static_cast<float>(geometry.bleed.up + geometry.bleed.down));
    return geometry;
  }

  std::int32_t
  adjustedOffsetX(std::int32_t baseOffset, const Geometry& geometry, HorizontalAttachment attachment) noexcept {
    switch (attachment) {
    case HorizontalAttachment::Left:
      return baseOffset - geometry.bleed.left;
    case HorizontalAttachment::Right:
      return baseOffset + geometry.bleed.right;
    case HorizontalAttachment::Center:
      return baseOffset
          + static_cast<std::int32_t>(
                 std::lround(static_cast<float>(geometry.bleed.right - geometry.bleed.left) * 0.5f)
          );
    }
    return baseOffset;
  }

  std::int32_t
  adjustedOffsetY(std::int32_t baseOffset, const Geometry& geometry, VerticalAttachment attachment) noexcept {
    switch (attachment) {
    case VerticalAttachment::Top:
      return baseOffset - geometry.bleed.up;
    case VerticalAttachment::Bottom:
      return baseOffset + geometry.bleed.down;
    case VerticalAttachment::Center:
      return baseOffset
          + static_cast<std::int32_t>(std::lround(static_cast<float>(geometry.bleed.down - geometry.bleed.up) * 0.5f));
    }
    return baseOffset;
  }

  void applyToConfig(PopupSurfaceConfig& config, const Geometry& geometry, Attachment attachment) noexcept {
    config.width = geometry.surfaceWidth;
    config.height = geometry.surfaceHeight;
    config.offsetX = adjustedOffsetX(config.offsetX, geometry, attachment.horizontal);
    config.offsetY = adjustedOffsetY(config.offsetY, geometry, attachment.vertical);
  }

  void setContentInputRegion(PopupSurface& surface, const Geometry& geometry) {
    surface.setInputRegion({geometry.inputRect()});
  }

  RectNode* addShadow(
      Node& parent, const Geometry& geometry, const ShellConfig::ShadowConfig& shadow, float radius,
      float backgroundOpacity
  ) {
    if (!shell::surface_shadow::enabled(true, shadow)) {
      return nullptr;
    }

    auto shadowNode = std::make_unique<RectNode>();
    shadowNode->setStyle(
        shell::surface_shadow::style(
            shadow, backgroundOpacity, shell::surface_shadow::Shape{.radius = Radii{radius, radius, radius, radius}}
        )
    );
    const auto offset = shadowDirectionOffset(shadow.direction);
    shadowNode->setPosition(
        geometry.contentX() + static_cast<float>(offset.x), geometry.contentY() + static_cast<float>(offset.y)
    );
    shadowNode->setFrameSize(geometry.contentWidth, geometry.contentHeight);
    shadowNode->setZIndex(-1);
    return static_cast<RectNode*>(parent.addChild(std::move(shadowNode)));
  }

  Box* addCardBackground(Node& parent, const Geometry& geometry, float contentScale) {
    auto box = std::make_unique<Box>();
    box->setCardStyle(contentScale, 1.0f, Style::popupBordersEnabled());
    box->setRadius(Style::scaledRadiusLg(contentScale));
    box->setPosition(geometry.contentX(), geometry.contentY());
    box->setFrameSize(geometry.contentWidth, geometry.contentHeight);
    return static_cast<Box*>(parent.addChild(std::move(box)));
  }

} // namespace popup_chrome
