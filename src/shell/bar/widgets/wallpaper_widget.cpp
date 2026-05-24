#include "shell/bar/widgets/wallpaper_widget.h"

#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <memory>

WallpaperWidget::WallpaperWidget(wl_output* /*output*/, std::string barGlyphId) : m_barGlyphId(std::move(barGlyphId)) {}

void WallpaperWidget::create() {
  auto area = std::make_unique<InputArea>();
  area->setOnClick([this](const InputArea::PointerData& /*data*/) { requestPanelToggle("wallpaper"); });

  area->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = m_barGlyphId.empty() ? "wallpaper-selector" : m_barGlyphId,
          .glyphSize = Style::barGlyphSize * m_contentScale,
          .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
      })
  );

  setRoot(std::move(area));
}

void WallpaperWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  if (m_glyph == nullptr) {
    return;
  }
  m_glyph->setGlyphSize(Style::barGlyphSize * m_contentScale);
  m_glyph->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
  m_glyph->measure(renderer);
  if (auto* node = root(); node != nullptr) {
    node->setSize(m_glyph->width(), m_glyph->height());
  }
}
