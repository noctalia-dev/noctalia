#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

struct WidgetTransformBounds {
  float scaledWidth = 0.0f;
  float scaledHeight = 0.0f;
  float aabbWidth = 0.0f;
  float aabbHeight = 0.0f;
  float left = 0.0f;
  float top = 0.0f;
};

struct WidgetTransformSurfaceGeometry {
  std::uint32_t surfaceWidth = 1;
  std::uint32_t surfaceHeight = 1;
  std::int32_t marginLeft = 0;
  std::int32_t marginTop = 0;
};

struct WidgetTransformClampResult {
  float cx = 0.0f;
  float cy = 0.0f;
};

inline constexpr float kDesktopWidgetMinVisibleFraction = 0.5f;

inline WidgetTransformBounds
computeWidgetTransformBounds(float cx, float cy, float width, float height, float scale, float rotationRad) {
  WidgetTransformBounds bounds;
  const float clampedScale = std::max(0.01f, scale);
  const float scaledWidth = std::max(1.0f, width * clampedScale);
  const float scaledHeight = std::max(1.0f, height * clampedScale);
  const float cosTheta = std::cos(rotationRad);
  const float sinTheta = std::sin(rotationRad);

  bounds.scaledWidth = scaledWidth;
  bounds.scaledHeight = scaledHeight;
  bounds.aabbWidth = std::abs(scaledWidth * cosTheta) + std::abs(scaledHeight * sinTheta);
  bounds.aabbHeight = std::abs(scaledWidth * sinTheta) + std::abs(scaledHeight * cosTheta);
  bounds.left = cx - bounds.aabbWidth * 0.5f;
  bounds.top = cy - bounds.aabbHeight * 0.5f;
  return bounds;
}

inline WidgetTransformSurfaceGeometry
computeWidgetSurfaceGeometry(float cx, float cy, float width, float height, float scale, float rotationRad) {
  const WidgetTransformBounds bounds = computeWidgetTransformBounds(cx, cy, width, height, scale, rotationRad);
  WidgetTransformSurfaceGeometry geometry;
  geometry.surfaceWidth = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::ceil(bounds.aabbWidth)));
  geometry.surfaceHeight = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::ceil(bounds.aabbHeight)));
  geometry.marginLeft = static_cast<std::int32_t>(std::lround(cx - static_cast<float>(geometry.surfaceWidth) * 0.5f));
  geometry.marginTop = static_cast<std::int32_t>(std::lround(cy - static_cast<float>(geometry.surfaceHeight) * 0.5f));
  return geometry;
}

struct WidgetTransformClippedGeometry {
  std::uint32_t surfaceWidth = 1;
  std::uint32_t surfaceHeight = 1;
  std::int32_t marginLeft = 0;
  std::int32_t marginTop = 0;
  float contentOffsetX = 0.0f;
  float contentOffsetY = 0.0f;
};

inline WidgetTransformClippedGeometry computeClippedWidgetSurfaceGeometry(
    float cx, float cy, float width, float height, float scale, float rotationRad, float outputWidth, float outputHeight
) {
  const WidgetTransformBounds bounds = computeWidgetTransformBounds(cx, cy, width, height, scale, rotationRad);
  const float halfW = bounds.aabbWidth * 0.5f;
  const float halfH = bounds.aabbHeight * 0.5f;

  const float desiredLeft = cx - halfW;
  const float desiredTop = cy - halfH;
  const float desiredRight = cx + halfW;
  const float desiredBottom = cy + halfH;

  const float clippedLeft = std::max(0.0f, desiredLeft);
  const float clippedTop = std::max(0.0f, desiredTop);
  const float clippedRight = std::min(outputWidth, desiredRight);
  const float clippedBottom = std::min(outputHeight, desiredBottom);

  const float visibleWidth = std::max(1.0f, clippedRight - clippedLeft);
  const float visibleHeight = std::max(1.0f, clippedBottom - clippedTop);

  WidgetTransformClippedGeometry geometry;
  geometry.surfaceWidth = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::ceil(visibleWidth)));
  geometry.surfaceHeight = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::ceil(visibleHeight)));
  geometry.marginLeft = static_cast<std::int32_t>(std::lround(clippedLeft));
  geometry.marginTop = static_cast<std::int32_t>(std::lround(clippedTop));
  geometry.contentOffsetX = cx - static_cast<float>(geometry.marginLeft);
  geometry.contentOffsetY = cy - static_cast<float>(geometry.marginTop);
  return geometry;
}

inline WidgetTransformClampResult clampWidgetCenterToOutput(
    float cx, float cy, float width, float height, float scale, float rotationRad, float outputWidth,
    float outputHeight, float minVisibleFraction = 1.0f
) {
  const WidgetTransformBounds bounds = computeWidgetTransformBounds(0.0f, 0.0f, width, height, scale, rotationRad);
  const float halfWidth = bounds.aabbWidth * 0.5f;
  const float halfHeight = bounds.aabbHeight * 0.5f;
  const float visibleFraction = std::clamp(minVisibleFraction, 0.0f, 1.0f);
  const float minVisibleWidth = std::min(outputWidth, bounds.aabbWidth * visibleFraction);
  const float minVisibleHeight = std::min(outputHeight, bounds.aabbHeight * visibleFraction);

  WidgetTransformClampResult clamped{cx, cy};
  const float minCenterX = minVisibleWidth - halfWidth;
  const float maxCenterX = outputWidth - minVisibleWidth + halfWidth;
  const float minCenterY = minVisibleHeight - halfHeight;
  const float maxCenterY = outputHeight - minVisibleHeight + halfHeight;

  clamped.cx = std::clamp(cx, minCenterX, maxCenterX);
  clamped.cy = std::clamp(cy, minCenterY, maxCenterY);

  return clamped;
}
