#include "shell/bar/widgets/test_widget.h"

#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <memory>

TestWidget::TestWidget(wl_output* /*output*/) {}

void TestWidget::create() {
  auto area = ui::inputArea({});
  area->setOnClick([this](const InputArea::PointerData& /*data*/) { requestPanelToggle("test"); });

  area->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = "flask",
          .glyphSize = Style::baseGlyphSize * m_contentScale,
          .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
      })
  );

  setRoot(std::move(area));
}

void TestWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  if (m_glyph == nullptr) {
    return;
  }
  m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
  m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
  m_glyph->measure(renderer);
  auto* node = root();
  if (node != nullptr) {
    node->setSize(m_glyph->width(), m_glyph->height());
  }
}
