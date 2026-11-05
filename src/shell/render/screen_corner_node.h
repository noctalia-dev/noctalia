#pragma once

#include "render/scene/custom_draw_node.h"
#include "shell/render/shell_render_styles.h"

class ScreenCornerNode : public CustomDrawNode {
public:
  const ScreenCornerStyle& style() const noexcept { return m_style; }

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

  void draw(RenderBackend& backend, const CustomDrawContext& context) const override;

private:
  ScreenCornerStyle m_style;
};
