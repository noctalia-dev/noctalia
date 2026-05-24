#include "shell/bar/widgets/settings_widget.h"

#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <memory>

SettingsWidget::SettingsWidget(wl_output* /*output*/, std::string barGlyphId) : m_barGlyphId(std::move(barGlyphId)) {}

void SettingsWidget::create() {
  auto area = std::make_unique<InputArea>();
  area->setOnClick([](const InputArea::PointerData& /*data*/) { PanelManager::instance().openSettingsWindow(); });

  area->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = m_barGlyphId.empty() ? "settings" : m_barGlyphId,
          .glyphSize = Style::barGlyphSize * m_contentScale,
          .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
      })
  );

  setRoot(std::move(area));
}

void SettingsWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
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
