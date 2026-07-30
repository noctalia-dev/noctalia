#include "shell/bar/widgets/spacer_widget.h"

#include "render/scene/node.h"
#include "ui/builders.h"

SpacerWidget::SpacerWidget(bool verticalBar, Options options)
    : m_fixedLength(static_cast<float>(options.length)), m_verticalBar(verticalBar) {}

void SpacerWidget::create() {
  auto spacer = ui::node({});
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
