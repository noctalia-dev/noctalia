#pragma once

#include "render/core/color.h"
#include "render/scene/custom_draw_node.h"
#include "shell/render/shell_render_styles.h"

class EffectNode : public CustomDrawNode {
public:
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

  void draw(RenderBackend& backend, const CustomDrawContext& context) const override;

private:
  EffectStyle m_style;
};
