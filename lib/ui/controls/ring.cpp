#include "ui/controls/ring.h"

#include "render/scene/ring_node.h"

#include <algorithm>
#include <memory>

Ring::Ring() {
  m_ring = static_cast<RingNode*>(addChild(std::make_unique<RingNode>()));
  syncStyle();
  m_paletteConn = paletteChanged().connect([this] { syncStyle(); });
}

void Ring::setFill(const ColorSpec& color) {
  m_fillColor = color;
  syncStyle();
}

void Ring::setFill(const Color& color) { setFill(fixedColorSpec(color)); }

void Ring::setTrack(const ColorSpec& color) {
  m_trackColor = color;
  syncStyle();
}

void Ring::setTrack(const Color& color) { setTrack(fixedColorSpec(color)); }

void Ring::setThickness(float thickness) {
  m_thickness = std::max(thickness, 0.0F);
  syncStyle();
}

void Ring::setProgress(float progress) {
  // NaN compares false both ways and would pass through std::clamp; treat it as empty.
  m_progress = progress >= 0.0F ? std::min(progress, 1.0F) : 0.0F;
  syncStyle();
}

void Ring::setSize(float width, float height) {
  Node::setSize(width, height);
  m_ring->setFrameSize(width, height);
}

void Ring::syncStyle() {
  m_ring->setStyle(
      RingStyle{
          .fill = resolveColorSpec(m_fillColor),
          .track = resolveColorSpec(m_trackColor),
          .thickness = m_thickness,
          .progress = m_progress,
      }
  );
}
