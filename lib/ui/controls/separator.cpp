#include "ui/controls/separator.h"

#include "render/core/render_styles.h"
#include "render/scene/rect_node.h"
#include "ui/controls/flex.h"

#include <memory>

Separator::Separator() {
  m_rectStart = static_cast<RectNode*>(addChild(std::make_unique<RectNode>()));
  m_rectEnd = static_cast<RectNode*>(addChild(std::make_unique<RectNode>()));
  m_paletteConn = paletteChanged().connect([this] { applyPalette(); });
  applyPalette();
}

void Separator::setColor(const ColorSpec& color) {
  m_color = color;
  applyPalette();
}

void Separator::setThickness(float thickness) {
  m_thickness = thickness;
  markLayoutDirty();
}

void Separator::setSpacing(float spacing) {
  if (m_spacing == spacing) {
    return;
  }
  m_spacing = spacing;
  markLayoutDirty();
}

void Separator::setOrientation(SeparatorOrientation orientation) {
  if (m_orientation == orientation) {
    return;
  }
  m_orientation = orientation;
  markLayoutDirty();
}

void Separator::setGradientEdges(bool enabled) {
  if (m_gradientEdges == enabled) {
    return;
  }
  m_gradientEdges = enabled;
  applyPalette();
}

bool Separator::ruleIsHorizontal() const {
  if (m_orientation == SeparatorOrientation::HorizontalRule) {
    return true;
  }
  if (m_orientation == SeparatorOrientation::VerticalRule) {
    return false;
  }
  if (const auto* flex = dynamic_cast<const Flex*>(parent()); flex != nullptr) {
    return flex->direction() == FlexDirection::Vertical;
  }
  return true;
}

LayoutSize Separator::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  const bool horiz = ruleIsHorizontal();
  float w = 0.0F;
  float h = 0.0F;
  if (horiz) {
    h = m_thickness + 2.0F * m_spacing;
    if (constraints.hasExactWidth()) {
      w = constraints.maxWidth;
    } else {
      w = width() > 0.0F ? width() : m_thickness;
    }
  } else {
    w = m_thickness + 2.0F * m_spacing;
    if (constraints.hasExactHeight()) {
      h = constraints.maxHeight;
    } else {
      h = height() > 0.0F ? height() : m_thickness;
    }
  }
  setSize(w, h);
  doLayout(renderer);
  return constraints.constrain(LayoutSize{w, h});
}

void Separator::doLayout(Renderer& /*renderer*/) {
  const bool horiz = ruleIsHorizontal();
  if (horiz) {
    const float w = width() > 0.0F ? width() : (parent() != nullptr ? parent()->width() : 0.0F);
    setSize(w, m_thickness + 2.0F * m_spacing);
    const float halfW = w * 0.5F;
    m_rectStart->setPosition(0.0F, m_spacing);
    m_rectStart->setFrameSize(halfW, m_thickness);
    m_rectEnd->setPosition(halfW, m_spacing);
    m_rectEnd->setFrameSize(w - halfW, m_thickness);
  } else {
    const float lineH = height() > 0.0F ? height() : (parent() != nullptr ? parent()->height() : 0.0F);
    setSize(m_thickness + 2.0F * m_spacing, lineH);
    const float halfH = lineH * 0.5F;
    m_rectStart->setPosition(m_spacing, 0.0F);
    m_rectStart->setFrameSize(m_thickness, halfH);
    m_rectEnd->setPosition(m_spacing, halfH);
    m_rectEnd->setFrameSize(m_thickness, lineH - halfH);
  }

  applyPalette();
}

void Separator::applyPalette() {
  const bool horiz = ruleIsHorizontal();

  const Color opaque = resolveColorSpec(m_color);
  const Color startEdge = m_gradientEdges ? Color{opaque.r, opaque.g, opaque.b, 0.0F} : opaque;
  const Color endEdge = startEdge;
  const GradientDirection dir = horiz ? GradientDirection::Horizontal : GradientDirection::Vertical;

  m_rectStart->setStyle(
      RoundedRectStyle{
          .fill = startEdge,
          .border = clearColor(),
          .fillMode = FillMode::LinearGradient,
          .gradientDirection = dir,
          .gradientStops =
              {GradientStop{0.0F, startEdge}, GradientStop{0.0F, startEdge}, GradientStop{1.0F, opaque},
               GradientStop{1.0F, opaque}},
          .radius = 0.0F,
          .softness = 0.0F,
          .borderWidth = 0.0F,
      }
  );

  m_rectEnd->setStyle(
      RoundedRectStyle{
          .fill = opaque,
          .border = clearColor(),
          .fillMode = FillMode::LinearGradient,
          .gradientDirection = dir,
          .gradientStops =
              {GradientStop{0.0F, opaque}, GradientStop{0.0F, opaque}, GradientStop{1.0F, endEdge},
               GradientStop{1.0F, endEdge}},
          .radius = 0.0F,
          .softness = 0.0F,
          .borderWidth = 0.0F,
      }
  );
}
