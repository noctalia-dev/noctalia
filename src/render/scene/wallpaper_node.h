#pragma once

#include "config/config_types.h"
#include "render/core/color.h"
#include "render/core/texture_handle.h"
#include "render/core/wallpaper_types.h"
#include "render/scene/node.h"

#include <cstdint>

class WallpaperNode : public Node {
public:
  WallpaperNode() : Node(NodeType::Wallpaper) {}

  [[nodiscard]] TextureId texture1() const noexcept { return m_texture1; }
  [[nodiscard]] TextureId texture2() const noexcept { return m_texture2; }
  [[nodiscard]] WallpaperSourceKind sourceKind1() const noexcept { return m_sourceKind1; }
  [[nodiscard]] WallpaperSourceKind sourceKind2() const noexcept { return m_sourceKind2; }
  [[nodiscard]] const Color& sourceColor1() const noexcept { return m_sourceColor1; }
  [[nodiscard]] const Color& sourceColor2() const noexcept { return m_sourceColor2; }
  [[nodiscard]] float imageWidth1() const noexcept { return m_imageWidth1; }
  [[nodiscard]] float imageHeight1() const noexcept { return m_imageHeight1; }
  [[nodiscard]] float imageWidth2() const noexcept { return m_imageWidth2; }
  [[nodiscard]] float imageHeight2() const noexcept { return m_imageHeight2; }
  [[nodiscard]] float progress() const noexcept { return m_progress; }
  [[nodiscard]] WallpaperTransition transition() const noexcept { return m_transition; }
  [[nodiscard]] WallpaperFillMode fillMode() const noexcept { return m_fillMode; }
  [[nodiscard]] const Color& fillColor() const noexcept { return m_fillColor; }
  [[nodiscard]] const TransitionParams& transitionParams() const noexcept { return m_params; }
  [[nodiscard]] const WallpaperSpanParams& spanParams() const noexcept { return m_span; }

  void setTextures(
      TextureId texture1, TextureId texture2, float imageWidth1, float imageHeight1, float imageWidth2,
      float imageHeight2
  ) {
    setSources(
        WallpaperSourceKind::Image, texture1, rgba(0.0f, 0.0f, 0.0f, 1.0f), WallpaperSourceKind::Image, texture2,
        rgba(0.0f, 0.0f, 0.0f, 1.0f), imageWidth1, imageHeight1, imageWidth2, imageHeight2
    );
  }

  void setSources(
      WallpaperSourceKind sourceKind1, TextureId texture1, const Color& sourceColor1, WallpaperSourceKind sourceKind2,
      TextureId texture2, const Color& sourceColor2, float imageWidth1, float imageHeight1, float imageWidth2,
      float imageHeight2
  ) {
    if (m_texture1 == texture1
        && m_texture2 == texture2
        && m_imageWidth1 == imageWidth1
        && m_imageHeight1 == imageHeight1
        && m_imageWidth2 == imageWidth2
        && m_imageHeight2 == imageHeight2
        && m_sourceKind1 == sourceKind1
        && m_sourceKind2 == sourceKind2
        && m_sourceColor1 == sourceColor1
        && m_sourceColor2 == sourceColor2) {
      return;
    }
    m_sourceKind1 = sourceKind1;
    m_sourceKind2 = sourceKind2;
    m_texture1 = texture1;
    m_texture2 = texture2;
    m_sourceColor1 = sourceColor1;
    m_sourceColor2 = sourceColor2;
    m_imageWidth1 = imageWidth1;
    m_imageHeight1 = imageHeight1;
    m_imageWidth2 = imageWidth2;
    m_imageHeight2 = imageHeight2;
    markPaintDirty();
  }

  void setTransition(WallpaperTransition transition, float progress, const TransitionParams& params) {
    if (m_transition == transition
        && m_progress == progress
        && m_params.direction == params.direction
        && m_params.centerX == params.centerX
        && m_params.centerY == params.centerY
        && m_params.stripeCount == params.stripeCount
        && m_params.angle == params.angle
        && m_params.maxBlockSize == params.maxBlockSize
        && m_params.cellSize == params.cellSize
        && m_params.smoothness == params.smoothness
        && m_params.aspectRatio == params.aspectRatio) {
      return;
    }
    m_transition = transition;
    m_progress = progress;
    m_params = params;
    markPaintDirty();
  }

  void setFillMode(WallpaperFillMode fillMode) {
    if (m_fillMode == fillMode) {
      return;
    }
    m_fillMode = fillMode;
    markPaintDirty();
  }

  void setFillColor(const Color& fillColor) {
    if (m_fillColor == fillColor) {
      return;
    }
    m_fillColor = fillColor;
    markPaintDirty();
  }

  void setSpan(const WallpaperSpanParams& span) {
    if (m_span == span) {
      return;
    }
    m_span = span;
    markPaintDirty();
  }

private:
  WallpaperSourceKind m_sourceKind1 = WallpaperSourceKind::Image;
  WallpaperSourceKind m_sourceKind2 = WallpaperSourceKind::Image;
  TextureId m_texture1;
  TextureId m_texture2;
  Color m_sourceColor1 = rgba(0.0f, 0.0f, 0.0f, 1.0f);
  Color m_sourceColor2 = rgba(0.0f, 0.0f, 0.0f, 1.0f);
  float m_imageWidth1 = 0.0f;
  float m_imageHeight1 = 0.0f;
  float m_imageWidth2 = 0.0f;
  float m_imageHeight2 = 0.0f;
  float m_progress = 0.0f;
  WallpaperTransition m_transition = WallpaperTransition::Fade;
  WallpaperFillMode m_fillMode = WallpaperFillMode::Crop;
  Color m_fillColor = rgba(0.0f, 0.0f, 0.0f, 1.0f);
  TransitionParams m_params;
  WallpaperSpanParams m_span;
};
