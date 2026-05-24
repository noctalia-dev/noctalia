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

void Separator::setOrientation(SeparatorOrientation orientation) {
  if (m_orientation == orientation) {
    return;
  }
  m_orientation = orientation;
  markLayoutDirty();
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
  float w = 0.0f;
  float h = 0.0f;
  if (horiz) {
    h = m_thickness;
    if (constraints.hasExactWidth()) {
      w = constraints.maxWidth;
    } else {
      w = width() > 0.0f ? width() : m_thickness;
    }
  } else {
    w = m_thickness;
    if (constraints.hasExactHeight()) {
      h = constraints.maxHeight;
    } else {
      h = height() > 0.0f ? height() : m_thickness;
    }
  }
  setSize(w, h);
  doLayout(renderer);
  return constraints.constrain(LayoutSize{w, h});
}

void Separator::doLayout(Renderer& /*renderer*/) {
  const bool horiz = ruleIsHorizontal();
  if (horiz) {
    const float w = width() > 0.0f ? width() : (parent() != nullptr ? parent()->width() : 0.0f);
    setSize(w, m_thickness);
    const float halfW = w * 0.5f;
    m_rectStart->setPosition(0.0f, 0.0f);
    m_rectStart->setFrameSize(halfW, m_thickness);
    m_rectEnd->setPosition(halfW, 0.0f);
    m_rectEnd->setFrameSize(w - halfW, m_thickness);
  } else {
    const float lineH = height() > 0.0f ? height() : (parent() != nullptr ? parent()->height() : 0.0f);
    setSize(m_thickness, lineH);
    const float halfH = lineH * 0.5f;
    m_rectStart->setPosition(0.0f, 0.0f);
    m_rectStart->setFrameSize(m_thickness, halfH);
    m_rectEnd->setPosition(0.0f, halfH);
    m_rectEnd->setFrameSize(m_thickness, lineH - halfH);
  }

  applyPalette();
}

void Separator::applyPalette() {
  const bool horiz = ruleIsHorizontal();

  const Color opaque = resolveColorSpec(m_color);
  Color transparent = opaque;
  transparent.a = 0.0f;
  const GradientDirection dir = horiz ? GradientDirection::Horizontal : GradientDirection::Vertical;

  m_rectStart->setStyle(
      RoundedRectStyle{
          .fill = transparent,
          .border = clearColor(),
          .fillMode = FillMode::LinearGradient,
          .gradientDirection = dir,
          .gradientStops =
              {GradientStop{0.0f, transparent}, GradientStop{0.0f, transparent}, GradientStop{1.0f, opaque},
               GradientStop{1.0f, opaque}},
          .radius = 0.0f,
          .softness = 0.0f,
          .borderWidth = 0.0f,
      }
  );

  m_rectEnd->setStyle(
      RoundedRectStyle{
          .fill = opaque,
          .border = clearColor(),
          .fillMode = FillMode::LinearGradient,
          .gradientDirection = dir,
          .gradientStops =
              {GradientStop{0.0f, opaque}, GradientStop{0.0f, opaque}, GradientStop{1.0f, transparent},
               GradientStop{1.0f, transparent}},
          .radius = 0.0f,
          .softness = 0.0f,
          .borderWidth = 0.0f,
      }
  );
}
