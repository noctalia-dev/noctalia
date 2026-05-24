#include "shell/bar/widgets/session_widget.h"

#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <memory>

SessionWidget::SessionWidget(wl_output* /*output*/, std::string barGlyphId) : m_barGlyphId(std::move(barGlyphId)) {}

void SessionWidget::create() {
  auto area = std::make_unique<InputArea>();
  area->setOnClick([this](const InputArea::PointerData& /*data*/) { requestPanelToggle("session"); });

  area->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = m_barGlyphId.empty() ? "shutdown" : m_barGlyphId,
          .glyphSize = Style::barGlyphSize * m_contentScale,
          .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
      })
  );

  setRoot(std::move(area));
}

void SessionWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  if (m_glyph == nullptr) {
    return;
  }
  m_glyph->setGlyphSize(Style::barGlyphSize * m_contentScale);
  m_glyph->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
  m_glyph->measure(renderer);
  auto* node = root();
  if (node != nullptr) {
    node->setSize(m_glyph->width(), m_glyph->height());
  }
}
