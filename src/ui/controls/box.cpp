#include "ui/controls/box.h"

#include "render/core/render_styles.h"
#include "render/scene/rect_node.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <memory>

Box::Box() {
  auto rect = std::make_unique<RectNode>();
  m_rect = static_cast<RectNode*>(addChild(std::move(rect)));
  m_style = m_rect->style();
  m_paletteConn = paletteChanged().connect([this] { applyPalette(); });
}

const RoundedRectStyle& Box::style() const noexcept { return m_style; }

void Box::setStyle(const RoundedRectStyle& style) {
  m_style = style;
  m_resolveFill = false;
  m_resolveBorder = false;
  m_borderWidth = style.borderWidth;
  syncStyle();
}

void Box::setFill(const ColorSpec& color) {
  m_fill = color;
  m_resolveFill = true;
  applyPalette();
}

void Box::setFill(const Color& color) { setFill(fixedColorSpec(color)); }

void Box::setBorder(const ColorSpec& color, float width) {
  m_border = color;
  m_borderWidth = width;
  m_resolveBorder = true;
  applyPalette();
}

void Box::setBorder(const Color& color, float width) { setBorder(fixedColorSpec(color), width); }

void Box::clearBorder() {
  m_border = clearColorSpec();
  m_borderWidth = 0.0f;
  m_resolveBorder = true;
  applyPalette();
}

void Box::setRadius(float radius) {
  m_style.radius = radius;
  syncStyle();
}

void Box::setRadii(const Radii& radii) {
  m_style.radius = radii;
  syncStyle();
}

void Box::setCornerShapes(const CornerShapes& corners) {
  m_style.corners = corners;
  syncStyle();
}

void Box::setLogicalInset(const RectInsets& inset) {
  m_style.logicalInset = inset;
  syncStyle();
}

void Box::setSoftness(float softness) {
  m_style.softness = softness;
  syncStyle();
}

void Box::setNoAa(bool noAa) {
  m_style.noAa = noAa;
  syncStyle();
}

void Box::setSize(float w, float h) {
  Node::setSize(w, h);
  m_rect->setFrameSize(w, h);
}

void Box::setFrameSize(float w, float h) {
  Node::setFrameSize(w, h);
  m_rect->setFrameSize(w, h);
}

void Box::applyPalette() {
  if (m_resolveFill) {
    m_style.fill = resolveColorSpec(m_fill);
    m_style.fillMode = FillMode::Solid;
  }
  if (m_resolveBorder) {
    m_style.border = resolveColorSpec(m_border);
    m_style.borderWidth = m_borderWidth;
  }
  syncStyle();
}

void Box::setFlatStyle() {
  m_fill = colorSpecFromRole(ColorRole::Surface);
  m_border = colorSpecFromRole(ColorRole::Outline);
  m_borderWidth = 0.0f;
  m_resolveFill = true;
  m_resolveBorder = true;
  m_style.fill = resolveColorSpec(m_fill);
  m_style.border = resolveColorSpec(m_border);
  m_style.borderWidth = m_borderWidth;
  m_style.fillMode = FillMode::Solid;
  m_style.corners = {};
  m_style.logicalInset = {};
  m_style.radius = 0;
  m_style.softness = 0;
  syncStyle();
}

void Box::setPanelStyle(bool showBorder) {
  m_fill = colorSpecFromRole(ColorRole::Surface);
  if (showBorder) {
    m_border = colorSpecFromRole(ColorRole::Outline);
    m_borderWidth = Style::borderWidth;
  } else {
    m_border = clearColorSpec();
    m_borderWidth = 0.0f;
  }
  m_resolveFill = true;
  m_resolveBorder = true;
  m_style.fill = resolveColorSpec(m_fill);
  m_style.border = resolveColorSpec(m_border);
  m_style.borderWidth = m_borderWidth;
  m_style.fillMode = FillMode::Solid;
  m_style.corners = {};
  m_style.logicalInset = {};
  m_style.radius = Style::scaledRadiusXl();
  m_style.softness = 1.0f;
  syncStyle();
}

void Box::setDialogStyle() { setPanelStyle(/*showBorder=*/true); }

void Box::setCardStyle(float scale, float fillOpacity, bool showBorder) {
  setFill(colorSpecFromRole(ColorRole::SurfaceVariant, fillOpacity));
  if (showBorder) {
    setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth);
  } else {
    clearBorder();
  }
  setRadius(Style::scaledRadiusXl(scale));
}

void Box::syncStyle() { m_rect->setStyle(m_style); }
