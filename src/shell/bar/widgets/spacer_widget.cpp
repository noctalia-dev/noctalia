#include "shell/bar/widgets/spacer_widget.h"

#include "render/scene/node.h"

SpacerWidget::SpacerWidget(float length, bool verticalBar) : m_fixedLength(length), m_verticalBar(verticalBar) {}

void SpacerWidget::create() {
  auto spacer = std::make_unique<Node>();
  spacer->setSize(0.0f, 0.0f);
  spacer->setHitTestVisible(false);
  setRoot(std::move(spacer));
}

void SpacerWidget::doLayout(Renderer& /*renderer*/, float /*containerWidth*/, float /*containerHeight*/) {
  if (root() != nullptr) {
    const float length = m_fixedLength * m_contentScale;
    root()->setSize(m_verticalBar ? 0.0f : length, m_verticalBar ? length : 0.0f);
  }
}
