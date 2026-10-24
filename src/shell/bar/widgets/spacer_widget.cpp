#include "shell/bar/widgets/spacer_widget.h"

#include "render/scene/node.h"
#include "ui/builders.h"

SpacerWidget::SpacerWidget(bool verticalBar, Options options)
    : m_fixedLength(static_cast<float>(options.length)), m_verticalBar(verticalBar), m_expand(options.expand) {}

void SpacerWidget::create() {
  auto spacer = ui::node({});
  spacer->setSize(0.0F, 0.0F);
  spacer->setHitTestVisible(false);
  setRoot(std::move(spacer));
}

void SpacerWidget::doLayout(Renderer& /*renderer*/, float /*containerWidth*/, float /*containerHeight*/) {
  if (root() != nullptr) {
    // An expanding spacer has no length of its own: the lane's flex layout hands it its share.
    const float length = m_expand ? 0.0F : m_fixedLength * m_contentScale;
    root()->setSize(m_verticalBar ? 0.0F : length, m_verticalBar ? length : 0.0F);
  }
}
