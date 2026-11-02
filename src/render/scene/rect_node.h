#pragma once

#include "render/core/render_styles.h"
#include "render/scene/node.h"

class RectNode : public Node {
public:
  RectNode() : Node(NodeType::Rect) {}

  const RoundedRectStyle& style() const noexcept { return m_style; }

  void setStyle(const RoundedRectStyle& style) {
    if (m_style == style) {
      return;
    }
    m_style = style;
    markPaintDirty();
  }

private:
  RoundedRectStyle m_style;
};
