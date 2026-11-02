#pragma once

#include "render/core/color.h"
#include "render/scene/custom_draw_node.h"
#include "shell/render/shell_render_styles.h"

#include <algorithm>

class CountdownRingNode : public CustomDrawNode {
public:
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
    const float clamped = std::clamp(progress, 0.0F, 1.0F);
    if (m_style.progress == clamped) {
      return;
    }
    m_style.progress = clamped;
    markPaintDirty();
  }

  const CountdownRingStyle& style() const noexcept { return m_style; }

  void draw(RenderBackend& backend, const CustomDrawContext& context) const override;

private:
  CountdownRingStyle m_style;
};
