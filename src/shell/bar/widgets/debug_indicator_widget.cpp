#ifndef NDEBUG

#include "shell/bar/widgets/debug_indicator_widget.h"

#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <utility>

DebugIndicatorWidget::DebugIndicatorWidget() = default;

void DebugIndicatorWidget::create() {
  auto container = ui::inputArea(
      {
          .out = &m_container,
          .acceptedButtons = 0,
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
          .fontSize = Style::fontSizeBody * m_contentScale,
          .fontWeight = labelFontWeight(),
          .fontFamily = labelFontFamily(),
          .color = colorSpecFromRole(ColorRole::Error),
          .maxLines = 1,
      })
  );
  setRoot(std::move(container));
}

void DebugIndicatorWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  if (m_container == nullptr || m_glyph == nullptr || m_label == nullptr) {
    return;
  }

  const bool isVertical = containerHeight > containerWidth;
  const float gap = Style::spaceXs * m_contentScale;
  m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
  m_glyph->setColor(colorSpecFromRole(ColorRole::Error));
  m_glyph->measure(renderer);
  m_label->setVisible(!isVertical);
  m_label->setFontSize(Style::fontSizeBody * m_contentScale);
  m_label->setColor(colorSpecFromRole(ColorRole::Error));
  m_label->setFontWeight(labelFontWeight());

  if (isVertical) {
    m_glyph->setPosition(0.0F, 0.0F);
    m_container->setSize(m_glyph->width(), m_glyph->height());
    return;
  }

  m_label->measure(renderer);
  const float rowHeight = std::max(m_glyph->height(), m_label->height());
  m_glyph->setPosition(0.0F, (rowHeight - m_glyph->height()) * 0.5F);
  m_label->setPosition(m_glyph->width() + gap, (rowHeight - m_label->height()) * 0.5F);
  m_container->setSize(m_glyph->width() + gap + m_label->width(), rowHeight);
}

#endif
