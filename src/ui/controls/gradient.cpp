#include "ui/controls/gradient.h"

#include "render/core/render_styles.h"
#include "render/scene/rect_node.h"

#include <memory>

Gradient::Gradient() {
  m_rect = static_cast<RectNode*>(addChild(std::make_unique<RectNode>()));
  m_paletteConn = paletteChanged().connect([this] { applyPalette(); });
  applyPalette();
}

const RoundedRectStyle& Gradient::style() const noexcept { return m_rect->style(); }

void Gradient::setGradient(float angleDeg, const std::array<GradientColorStop, 4>& stops) {
  if (m_angleDeg == angleDeg && m_stops == stops) {
    return;
  }
  m_angleDeg = angleDeg;
  m_stops = stops;
  applyPalette();
}

void Gradient::clearGradient() {
  if (m_stops == std::array<GradientColorStop, 4>{} && m_offset == 0.0F) {
    return;
  }
  m_stops = {};
  m_offset = 0.0F;
  applyPalette();
}

void Gradient::setOffset(float offset) {
  if (m_offset == offset) {
    return;
  }
  m_offset = offset;
  applyPalette();
}

void Gradient::setRadius(float radius) {
  if (m_radius == radius) {
    return;
  }
  m_radius = radius;
  applyPalette();
}

void Gradient::setSoftness(float softness) {
  if (m_softness == softness) {
    return;
  }
  m_softness = softness;
  applyPalette();
}

void Gradient::setSize(float width, float height) {
  Node::setSize(width, height);
  m_rect->setFrameSize(width, height);
  m_rect->setPosition(0.0F, 0.0F);
}

void Gradient::applyPalette() {
  std::array<GradientStop, 4> resolved{};
  for (std::size_t i = 0; i < m_stops.size(); ++i) {
    resolved[i] = GradientStop{.position = m_stops[i].position, .color = resolveColorSpec(m_stops[i].color)};
  }
  RoundedRectStyle style{.fillMode = FillMode::LinearGradient, .gradientAngleDeg = m_angleDeg};
  style.gradientOffset = m_offset;
  style.gradientStops = resolved;
  style.radius = m_radius;
  style.softness = m_softness;
  m_rect->setStyle(style);
}
