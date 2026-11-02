#pragma once

#include "render/core/color.h"
#include "render/core/render_styles.h"
#include "render/scene/node.h"

class SpinnerNode : public Node {
public:
  SpinnerNode() : Node(NodeType::Spinner) {}

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

  const Color& color() const noexcept { return m_style.color; }
  float thickness() const noexcept { return m_style.thickness; }
  const SpinnerStyle& style() const noexcept { return m_style; }

private:
  SpinnerStyle m_style;
};
