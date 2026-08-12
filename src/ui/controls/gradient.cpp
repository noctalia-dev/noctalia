#include "ui/controls/gradient.h"

#include "render/animation/animation.h"
#include "render/animation/motion_service.h"
#include "render/core/render_styles.h"
#include "render/scene/rect_node.h"

#include <cmath>
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

void Gradient::setMotion(GradientMotion motion, float durationMs, float from, float to) {
  if (!std::isfinite(durationMs) || durationMs <= 0.0F || !std::isfinite(from) || !std::isfinite(to)) {
    motion = GradientMotion::None;
  }
  if (motion != GradientMotion::None && from == to) {
    motion = GradientMotion::None;
    setOffset(from); // a zero-width trip still pins the shared endpoint
  }
  if (m_motion == motion && m_motionDurationMs == durationMs && m_motionFrom == from && m_motionTo == to) {
    return; // unchanged configuration keeps the running trip and its phase
  }
  stopTrip();
  m_motion = motion;
  m_motionDurationMs = durationMs;
  m_motionFrom = from;
  m_motionTo = to;
  m_outbound = true;
  startTrip();
}

void Gradient::setMotionVisible(bool visible) {
  if (m_motionVisible == visible) {
    return;
  }
  m_motionVisible = visible;
  if (!m_motionVisible) {
    stopTrip();
    m_outbound = true;
  } else {
    startTrip();
  }
}

void Gradient::setAnimationManager(AnimationManager* manager) {
  stopTrip();
  Node::setAnimationManager(manager);
  m_outbound = true;
  startTrip();
}

void Gradient::stopTrip() {
  if (m_motionId != 0) {
    if (AnimationManager* manager = animationManager()) {
      manager->cancel(m_motionId);
    }
    m_motionId = 0;
  }
}

void Gradient::startTrip() {
  if (m_motion == GradientMotion::None || !m_motionVisible) {
    return;
  }
  AnimationManager* manager = animationManager();
  if (manager == nullptr) {
    return;
  }
  if (!MotionService::instance().enabled()) {
    // animate() would build a 1 ms completion whose onComplete re-enters this
    // method and chains forever; park at the midpoint instead.
    setOffset((m_motionFrom + m_motionTo) * 0.5F);
    return;
  }
  const float tripFrom = m_outbound ? m_motionFrom : m_motionTo;
  const float tripTo = m_outbound ? m_motionTo : m_motionFrom;
  // ponytail: linear easing for loop and ping-pong until upstream picks a
  // ping-pong easing; swap the constant here when they do.
  m_motionId = manager->animate(
      tripFrom, tripTo, m_motionDurationMs, Easing::Linear, [this](float value) { setOffset(value); },
      [this] {
        m_motionId = 0;
        if (m_motion == GradientMotion::PingPong) {
          m_outbound = !m_outbound;
        }
        startTrip();
      },
      this
  );
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
