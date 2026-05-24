#ifndef NDEBUG

#include "shell/bar/widgets/debug_indicator_widget.h"

#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

DebugIndicatorWidget::DebugIndicatorWidget() = default;

void DebugIndicatorWidget::create() {
  setRoot(
      ui::chip({
          .out = &m_chip,
          .text = "DEBUG",
          .configure = [this](Chip& chip) {
            chip.setFill(colorSpecFromRole(ColorRole::Error));
            chip.label()->setColor(colorSpecFromRole(ColorRole::OnError));
            chip.label()->setFontWeight(labelFontWeight());
            chip.clearBorder();
            chip.setPadding(2.0f, Style::spaceSm);
          },
      })
  );
}

void DebugIndicatorWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  const LayoutConstraints unconstrained{};
  const auto size = m_chip->measure(renderer, unconstrained);
  m_chip->setRadius(std::min(size.width, size.height) * 0.5f);
  root()->setSize(size.width, size.height);
}

#endif
