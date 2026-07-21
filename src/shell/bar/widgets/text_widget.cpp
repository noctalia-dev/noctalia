#include "shell/bar/widgets/text_widget.h"

#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <memory>
#include <utility>

TextWidget::TextWidget(std::string text) : m_text(std::move(text)) {}

void TextWidget::create() {
  auto root = std::make_unique<Node>();
  root->addChild(
      ui::label({
          .out = &m_label,
          .text = m_text,
          .fontSize = Style::fontSizeBody * m_contentScale,
          .fontWeight = labelFontWeight(),
          .fontFamily = labelFontFamily(),
          .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
          .maxLines = 1,
          .visible = !m_text.empty(),
      })
  );
  setRoot(std::move(root));
}

void TextWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  auto* rootNode = root();
  if (m_label == nullptr || rootNode == nullptr) {
    return;
  }

  if (m_text.empty()) {
    m_label->setVisible(false);
    rootNode->setSize(0.0f, 0.0f);
    return;
  }

  const bool isVertical = containerHeight > containerWidth;
  m_label->setVisible(true);
  m_label->setFontSize((isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * m_contentScale);
  m_label->setFontWeight(labelFontWeight());
  m_label->setFontFamily(labelFontFamily());
  m_label->setTextAlign(isVertical ? TextAlign::Center : TextAlign::Start);
  m_label->setMaxWidth(isVertical ? containerWidth : 0.0f);
  m_label->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
  m_label->setText(m_text);
  m_label->measure(renderer);

  const float width = m_label->width();
  const float height = m_label->height();
  m_label->setPosition(0.0f, 0.0f);
  rootNode->setSize(width, height);
}
