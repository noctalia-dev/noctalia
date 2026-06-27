#include "shell/bar/widgets/idle_inhibitor_widget.h"

#include "idle/idle_inhibitor.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <memory>

namespace {

  const char* glyphForState(bool enabled) { return enabled ? "caffeine-on" : "caffeine-off"; }

} // namespace

IdleInhibitorWidget::IdleInhibitorWidget(IdleInhibitor* inhibitor) : m_inhibitor(inhibitor) {}

void IdleInhibitorWidget::create() {
  auto area = std::make_unique<InputArea>();
  area->setOnClick([this](const InputArea::PointerData& /*data*/) {
    if (m_inhibitor != nullptr && m_inhibitor->available()) {
      m_inhibitor->toggle();
    }
  });
  m_area = area.get();

  area->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = glyphForState(false),
          .glyphSize = Style::baseGlyphSize * m_contentScale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );

  setRoot(std::move(area));
}

void IdleInhibitorWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
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

void IdleInhibitorWidget::doUpdate(Renderer& renderer) { syncState(renderer); }

void IdleInhibitorWidget::syncState(Renderer& renderer) {
  if (m_glyph == nullptr || m_area == nullptr) {
    return;
  }

  const bool available = m_inhibitor != nullptr && m_inhibitor->available();
  const bool enabled = available && m_inhibitor->enabled();

  if (available == m_lastAvailable && enabled == m_lastEnabled) {
    return;
  }

  m_lastAvailable = available;
  m_lastEnabled = enabled;

  m_glyph->setGlyph(glyphForState(enabled));
  m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
  if (!available) {
    m_glyph->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  } else if (enabled) {
    m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::Primary)));
  } else {
    m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
  }
  m_glyph->measure(renderer);
  m_area->setEnabled(available);
  if (auto* node = root(); node != nullptr) {
    node->setOpacity(available ? 1.0f : 0.55f);
  }
  requestRedraw();
}
