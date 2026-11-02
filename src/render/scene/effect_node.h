#pragma once

#include "render/core/color.h"
#include "render/core/render_styles.h"
#include "render/scene/node.h"

class EffectNode : public Node {
public:
  EffectNode() : Node(NodeType::Effect) {}

  const EffectStyle& style() const noexcept { return m_style; }

  void setEffectType(EffectType type) {
    if (m_style.type == type) {
      return;
    }
    m_style.type = type;
    markPaintDirty();
  }

  void setTime(float time) {
    m_style.time = time;
    markPaintDirty();
  }

  void setRadius(float radius) {
    if (m_style.radius == radius) {
      return;
    }
    m_style.radius = radius;
    markPaintDirty();
  }

  void setBgColor(const Color& color) {
    if (m_style.bgColor == color) {
      return;
    }
    m_style.bgColor = color;
    markPaintDirty();
  }

private:
  EffectStyle m_style;
};
