#include "ui/controls/countdown_ring.h"

#include "render/core/renderer.h"
#include "render/scene/countdown_ring_node.h"
#include "ui/controls/label.h"
#include "ui/palette.h"

#include <algorithm>
#include <memory>

namespace {

  constexpr float kDefaultRingSize = 64.0f;
  constexpr float kDefaultThickness = 6.0f;

} // namespace

CountdownRing::CountdownRing() {
  m_paletteConn = paletteChanged().connect([this] { applyPalette(); });

  auto ringNode = std::make_unique<CountdownRingNode>();
  ringNode->setThickness(kDefaultThickness);
  ringNode->setParticipatesInLayout(false);
  ringNode->setZIndex(0);
  m_ringNode = static_cast<CountdownRingNode*>(addChild(std::move(ringNode)));

  auto secondsLabel = std::make_unique<Label>();
  secondsLabel->setFontWeight(FontWeight::Bold);
  secondsLabel->setFontSize(kDefaultRingSize * 0.34f);
  secondsLabel->setTextAlign(TextAlign::Center);
  secondsLabel->setBaselineMode(LabelBaselineMode::Text);
  secondsLabel->setParticipatesInLayout(false);
  secondsLabel->setHitTestVisible(false);
  secondsLabel->setEnabled(false);
  secondsLabel->setFocusable(false);
  secondsLabel->setZIndex(1);
  m_secondsLabel = static_cast<Label*>(addChild(std::move(secondsLabel)));

  m_ringSize = kDefaultRingSize;
  m_thickness = kDefaultThickness;
  applyPalette();
  syncGeometry();
}

void CountdownRing::setRingSize(float size) {
  m_ringSize = std::max(16.0f, size);
  markLayoutDirty();
}

void CountdownRing::setThickness(float thickness) {
  m_thickness = std::max(2.0f, thickness);
  if (m_ringNode != nullptr) {
    m_ringNode->setThickness(m_thickness);
  }
  markLayoutDirty();
}

void CountdownRing::setFontSize(float size) {
  m_fontSize = std::max(8.0f, size);
  if (m_secondsLabel != nullptr) {
    m_secondsLabel->setFontSize(m_fontSize);
  }
  markLayoutDirty();
}

void CountdownRing::setProgress(float progress) {
  if (m_ringNode != nullptr) {
    m_ringNode->setProgress(progress);
  }
}

void CountdownRing::setSeconds(int seconds) {
  if (m_secondsLabel != nullptr) {
    m_secondsLabel->setText(std::to_string(std::max(0, seconds)));
    markLayoutDirty();
  }
}

void CountdownRing::setColor(const ColorSpec& color) {
  m_color = color;
  applyPalette();
}

void CountdownRing::setColor(const Color& color) { setColor(fixedColorSpec(color)); }

void CountdownRing::applyPalette() {
  const Color resolved = resolveColorSpec(m_color);
  if (m_ringNode != nullptr) {
    m_ringNode->setColor(resolved);
  }
  if (m_secondsLabel != nullptr) {
    m_secondsLabel->setColor(resolved);
  }
}

LayoutSize CountdownRing::doMeasure(Renderer& /*renderer*/, const LayoutConstraints& constraints) {
  LayoutSize size{m_ringSize, m_ringSize};
  return constraints.constrain(size);
}

void CountdownRing::doLayout(Renderer& renderer) {
  syncGeometry();

  if (m_secondsLabel == nullptr) {
    return;
  }

  m_secondsLabel->measure(renderer);
  const float labelW = m_secondsLabel->width();
  const float labelH = m_secondsLabel->height();
  const float labelX = (m_ringSize - labelW) * 0.5f;
  const float labelY = (m_ringSize - labelH) * 0.5f;
  m_secondsLabel->setPosition(labelX, labelY);
  m_secondsLabel->setFrameSize(labelW, labelH);
}

void CountdownRing::syncGeometry() {
  setSize(m_ringSize, m_ringSize);
  if (m_ringNode != nullptr) {
    m_ringNode->setFrameSize(m_ringSize, m_ringSize);
    m_ringNode->setPosition(0.0f, 0.0f);
  }
}
