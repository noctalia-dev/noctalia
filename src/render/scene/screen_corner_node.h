#pragma once

#include "render/core/render_styles.h"
#include "render/scene/node.h"

class ScreenCornerNode : public Node {
public:
  ScreenCornerNode() : Node(NodeType::ScreenCorner) {}

  [[nodiscard]] const ScreenCornerStyle& style() const noexcept { return m_style; }

  void setStyle(const ScreenCornerStyle& style) {
    if (m_style == style) {
      return;
    }
    m_style = style;
    markPaintDirty();
  }

  void setColor(const Color& color) {
    if (m_style.color == color) {
      return;
    }
    m_style.color = color;
    markPaintDirty();
  }

  void setCorner(ScreenCornerPosition position) {
    if (m_style.position == position) {
      return;
    }
    m_style.position = position;
    markPaintDirty();
  }

  void setExponent(float exponent) {
    if (m_style.exponent == exponent) {
      return;
    }
    m_style.exponent = exponent;
    markPaintDirty();
  }

private:
  ScreenCornerStyle m_style;
};
