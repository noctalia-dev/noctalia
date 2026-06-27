#pragma once

#include "render/core/color.h"
#include "render/core/render_styles.h"
#include "render/scene/node.h"

#include <algorithm>

class CountdownRingNode : public Node {
public:
  CountdownRingNode() : Node(NodeType::CountdownRing) {}

  void setColor(const Color& color) {
    if (m_style.color == color) {
      return;
    }
    m_style.color = color;
    markPaintDirty();
  }
  void setThickness(float thickness) {
    if (m_style.thickness == thickness) {
      return;
    }
    m_style.thickness = thickness;
    markPaintDirty();
  }
  void setProgress(float progress) {
    const float clamped = std::clamp(progress, 0.0f, 1.0f);
    if (m_style.progress == clamped) {
      return;
    }
    m_style.progress = clamped;
    markPaintDirty();
  }

  [[nodiscard]] const CountdownRingStyle& style() const noexcept { return m_style; }

private:
  CountdownRingStyle m_style;
};
