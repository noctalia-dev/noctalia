#include "shell/bar/widgets/nightlight_widget.h"

#include "render/scene/input_area.h"
#include "system/gamma_service.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <memory>

namespace {

  const char* glyphForState(bool enabled, bool forced) {
    if (forced) {
      return "nightlight-forced";
    }
    return enabled ? "nightlight-on" : "nightlight-off";
  }

} // namespace

NightLightWidget::NightLightWidget(GammaService* nightLight) : m_nightLight(nightLight) {}

void NightLightWidget::create() {
  auto area = ui::inputArea({});
  m_area = area.get();

  area->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = "nightlight-off",
          .glyphSize = Style::baseGlyphSize * m_contentScale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );

  setRoot(std::move(area));
}

void NightLightWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  if (m_glyph == nullptr) {
    return;
  }

  syncState(renderer);
  m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
  m_glyph->measure(renderer);

  if (auto* node = root(); node != nullptr) {
    node->setSize(m_glyph->width(), m_glyph->height());
  }
}

void NightLightWidget::doUpdate(Renderer& renderer) { syncState(renderer); }

void NightLightWidget::syncState(Renderer& renderer) {
  if (m_glyph == nullptr || m_area == nullptr) {
    return;
  }

  const bool enabled = m_nightLight != nullptr && m_nightLight->enabled();
  const bool active = m_nightLight != nullptr && m_nightLight->active();
  const bool forced = m_nightLight != nullptr && m_nightLight->forceEnabled();

  if (enabled == m_lastEnabled && active == m_lastActive && forced == m_lastForced) {
    return;
  }

  m_lastEnabled = enabled;
  m_lastActive = active;
  m_lastForced = forced;

  m_glyph->setGlyph(glyphForState(enabled, forced));
  m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);

  if (forced || (enabled && active)) {
    m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::Primary)));
  } else if (enabled) {
    m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
  } else {
    m_glyph->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  }

  m_glyph->measure(renderer);
  if (auto* node = root(); node != nullptr) {
    node->setOpacity(enabled || forced ? 1.0f : 0.55f);
  }
  requestRedraw();
}
