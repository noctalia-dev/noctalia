#pragma once

#include "render/core/render_styles.h"
#include "render/scene/node.h"
#include "ui/palette.h"

#include <optional>

class RectNode;

// A styled rectangle that keeps its internal RectNode sized to match itself.
// Use this anywhere you need a background or decorative shape in shell/widget
// code — not RectNode directly.
class Box : public Node {
public:
  Box();

  [[nodiscard]] const RoundedRectStyle& style() const noexcept;
  void setStyle(const RoundedRectStyle& style);

  void setFill(const ColorSpec& color);
  // Explicit fixed color.
  void setFill(const Color& color);
  void setBorder(const ColorSpec& color, float width);
  void setBorder(const Color& color, float width);
  void clearBorder();
  void setRadius(float radius);
  void setRadii(const Radii& radii);
  void setCornerShapes(const CornerShapes& corners);
  void setLogicalInset(const RectInsets& inset);
  void setSoftness(float softness);
  void setNoAa(bool noAa);

  // Presets
  void setFlatStyle();
  void setCardStyle(float scale = 1.0f, float fillOpacity = 1.0f, bool showBorder = true);
  void setPanelStyle(bool showBorder = true);
  // Dialog/popup background. Like the panel style but always carries a subtle
  // outline so the dialog separates from the parent surface it floats over,
  // independent of the [shell.panel].borders toggle.
  void setDialogStyle();

  void setSize(float width, float height) override;
  void setFrameSize(float width, float height);

private:
  void applyPalette();
  void syncStyle();

  RectNode* m_rect = nullptr;
  RoundedRectStyle m_style;
  ColorSpec m_fill = clearColorSpec();
  ColorSpec m_border = clearColorSpec();
  float m_borderWidth = 0.0f;
  bool m_resolveFill = true;
  bool m_resolveBorder = true;
  Signal<>::ScopedConnection m_paletteConn;
};
