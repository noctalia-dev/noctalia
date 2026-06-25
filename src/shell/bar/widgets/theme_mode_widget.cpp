#include "shell/bar/widgets/theme_mode_widget.h"

#include "render/scene/input_area.h"
#include "theme/theme_service.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <memory>

namespace {

  const char* glyphForMode(bool /*isLight*/) { return "theme-mode"; }

} // namespace

ThemeModeWidget::ThemeModeWidget(noctalia::theme::ThemeService* themeService) : m_themeService(themeService) {}

void ThemeModeWidget::create() {
  auto area = std::make_unique<InputArea>();
  area->setOnClick([this](const InputArea::PointerData& /*data*/) {
    if (m_themeService == nullptr) {
      return;
    }
    m_themeService->toggleLightDark();
    m_lastIsLight = !m_lastIsLight;
    if (m_glyph != nullptr) {
      m_glyph->setGlyph(glyphForMode(m_lastIsLight));
      m_glyph->setColor(
          m_lastIsLight ? widgetIconColorOr(colorSpecFromRole(ColorRole::Primary))
                        : widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface))
      );
    }
    requestRedraw();
  });
  m_area = area.get();

  area->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = "theme-mode",
          .glyphSize = Style::baseGlyphSize * m_contentScale,
          .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
      })
  );

  setRoot(std::move(area));
}

void ThemeModeWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  if (m_glyph == nullptr) {
    return;
  }

  syncState(renderer);
  m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
  m_glyph->setColor(
      m_lastIsLight ? widgetIconColorOr(colorSpecFromRole(ColorRole::Primary))
                    : widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface))
  );
  m_glyph->measure(renderer);

  if (auto* node = root(); node != nullptr) {
    node->setSize(m_glyph->width(), m_glyph->height());
  }
}

void ThemeModeWidget::doUpdate(Renderer& renderer) { syncState(renderer); }

void ThemeModeWidget::syncState(Renderer& renderer) {
  if (m_themeService == nullptr || m_glyph == nullptr || m_area == nullptr) {
    return;
  }

  const bool isLight = m_themeService->isLightMode();
  if (isLight == m_lastIsLight) {
    return;
  }

  m_lastIsLight = isLight;
  m_glyph->setGlyph(glyphForMode(isLight));
  m_glyph->setColor(
      isLight ? widgetIconColorOr(colorSpecFromRole(ColorRole::Primary))
              : widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface))
  );
  m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
  m_glyph->measure(renderer);

  if (auto* node = root(); node != nullptr) {
    node->setOpacity(isLight ? 1.0f : 0.85f);
  }
  requestRedraw();
}
