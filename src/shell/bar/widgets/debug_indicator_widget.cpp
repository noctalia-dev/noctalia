#ifndef NDEBUG

#include "shell/bar/widgets/debug_indicator_widget.h"

#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <utility>

DebugIndicatorWidget::DebugIndicatorWidget() = default;

void DebugIndicatorWidget::create() {
  auto row = ui::row(
      {
          .out = &m_container,
          .align = FlexAlign::Center,
          .gap = Style::spaceXs * m_contentScale,
      },
      ui::glyph({
          .out = &m_glyph,
          .glyph = "bug",
          .glyphSize = Style::baseGlyphSize * m_contentScale,
          .color = colorSpecFromRole(ColorRole::Error),
      }),
      ui::label({
          .out = &m_label,
          .text = "DEBUG",
          .fontSize = Style::fontSizeCaption * m_contentScale,
          .fontFamily = labelFontFamily(),
          .color = colorSpecFromRole(ColorRole::Error),
          .maxLines = 1,
          .fontWeight = labelFontWeight(),
      })
  );
  setRoot(std::move(row));
}

void DebugIndicatorWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  if (m_container == nullptr || m_glyph == nullptr || m_label == nullptr) {
    return;
  }

  const bool isVertical = containerHeight > containerWidth;
  m_container->setGap(Style::spaceXs * m_contentScale);
  m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
  m_glyph->setColor(colorSpecFromRole(ColorRole::Error));
  m_label->setVisible(!isVertical);
  m_label->setFontSize(Style::fontSizeCaption * m_contentScale);
  m_label->setColor(colorSpecFromRole(ColorRole::Error));
  m_label->setFontWeight(labelFontWeight());
  m_container->layout(renderer);
}

#endif
