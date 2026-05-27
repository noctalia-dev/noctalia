#include "ui/controls/screen_corner.h"

#include "render/scene/screen_corner_node.h"

#include <memory>

ScreenCorner::ScreenCorner() {
  auto corner = std::make_unique<ScreenCornerNode>();
  m_corner = dynamic_cast<ScreenCornerNode*>(addChild(std::move(corner)));
}

void ScreenCorner::setColor(const Color& color) { m_corner->setColor(color); }

void ScreenCorner::setCorner(ScreenCornerPosition position) { m_corner->setCorner(position); }

void ScreenCorner::setExponent(float exponent) { m_corner->setExponent(exponent); }

void ScreenCorner::setSoftness(float softness) { m_corner->setSoftness(softness); }

void ScreenCorner::setSize(float width, float height) {
  Node::setSize(width, height);
  m_corner->setFrameSize(width, height);
}

void ScreenCorner::setFrameSize(float width, float height) {
  Node::setFrameSize(width, height);
  m_corner->setFrameSize(width, height);
}
