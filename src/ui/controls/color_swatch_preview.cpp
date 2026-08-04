#include "ui/controls/color_swatch_preview.h"

#include "ui/controls/box.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <memory>

ColorSwatchPreviewStrip::ColorSwatchPreviewStrip() {
  setDirection(FlexDirection::Horizontal);
  setAlign(FlexAlign::Stretch);
  setGap(0.0F);
  setPadding(0.0F);
  setClipChildren(true);
  setHitTestVisible(false);
  setFill(colorSpecFromRole(ColorRole::Surface));
  setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth);

  for (auto& swatchPtr : m_swatches) {
    auto swatch = std::make_unique<Box>();
    swatch->clearBorder();
    swatch->setVisible(false);
    swatch->setParticipatesInLayout(false);
    swatchPtr = static_cast<Box*>(addChild(std::move(swatch)));
  }

  syncGeometry();
}

void ColorSwatchPreviewStrip::doLayout(Renderer& /*renderer*/) {
  positionSwatches(height() > 0.0F ? height() : m_height);
}

LayoutSize ColorSwatchPreviewStrip::doMeasure(Renderer& /*renderer*/, const LayoutConstraints& constraints) {
  syncGeometry();
  return constraints.constrain(LayoutSize{.width = preferredWidth(), .height = m_height});
}

void ColorSwatchPreviewStrip::doArrange(Renderer& /*renderer*/, const LayoutRect& rect) {
  setPosition(rect.x, rect.y);
  setFrameSize(rect.width, rect.height);
  positionSwatches(rect.height);
}

void ColorSwatchPreviewStrip::setPreview(const ColorSwatchPreview& preview) {
  setFill(preview.surface.value_or(colorSpecFromRole(ColorRole::Surface)));

  m_visibleSwatches = std::min(preview.swatches.size(), kMaxSwatches);
  for (std::size_t i = 0; i < m_swatches.size(); ++i) {
    auto* swatch = m_swatches[i];
    if (swatch == nullptr) {
      continue;
    }
    const bool visible = i < m_visibleSwatches;
    if (visible) {
      swatch->setFill(preview.swatches[i]);
    }
    swatch->setVisible(visible);
    swatch->setParticipatesInLayout(visible);
  }
  syncGeometry();
}

void ColorSwatchPreviewStrip::setMetricsFromFontSize(float fontSize) {
  const float size = std::max(1.0F, fontSize);
  m_discSize = std::max(9.0F, std::round(size * 0.86F));
  m_gap = std::max(4.0F, std::round(size * 0.28F));
  m_paddingX = std::max(4.0F, std::round(size * 0.28F));
  m_paddingY = std::max(2.0F, std::round(size * 0.14F)) + 2.0F;
  m_height = m_discSize + m_paddingY * 2.0F;
  syncGeometry();
}

float ColorSwatchPreviewStrip::preferredWidth() const noexcept {
  if (m_visibleSwatches == 0) {
    return 0.0F;
  }
  return m_paddingX * 2.0F
      + m_discSize * static_cast<float>(m_visibleSwatches)
      + m_gap * static_cast<float>(m_visibleSwatches - 1);
}

void ColorSwatchPreviewStrip::syncGeometry() {
  for (auto* swatch : m_swatches) {
    if (swatch != nullptr) {
      swatch->setFrameSize(m_discSize, m_discSize);
    }
  }

  const float width = preferredWidth();
  const float capsuleRadius = m_height * 0.5F;
  setRadius(capsuleRadius);
  setMinWidth(width);
  setMinHeight(m_height);
  setFrameSize(width, m_height);

  positionSwatches(m_height);
}

void ColorSwatchPreviewStrip::positionSwatches(float height) {
  const float discRadius = m_discSize * 0.5F;
  for (std::size_t i = 0; i < m_swatches.size(); ++i) {
    auto* swatch = m_swatches[i];
    if (swatch == nullptr) {
      continue;
    }
    swatch->setPosition(
        m_paddingX + static_cast<float>(i) * (m_discSize + m_gap), std::round((height - m_discSize) * 0.5F)
    );
    swatch->setRadii(Radii{discRadius});
  }
}
