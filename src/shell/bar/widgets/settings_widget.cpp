#include "shell/bar/widgets/settings_widget.h"

#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/file_utils.h"

#include <memory>

namespace {

  WidgetCustomImage customImageFrom(const SettingsWidget::Options& options) {
    return {
        .path = FileUtils::expandUserPath(options.customImage).string(),
        .colorize = options.customImageColorize,
    };
  }

} // namespace

SettingsWidget::SettingsWidget(wl_output* /*output*/, Options options)
    : m_barGlyphId(options.glyph.empty() ? "search" : std::move(options.glyph)),
      m_customImage(customImageFrom(options)) {}

void SettingsWidget::create() {
  auto area = std::make_unique<InputArea>();
  area->setOnClick([](const InputArea::PointerData& /*data*/) { PanelManager::instance().openSettingsWindow(); });

  if (m_customImage.enabled()) {
    area->addChild(ui::image({.out = &m_image, .fit = ImageFit::Contain}));
  } else {
    area->addChild(
        ui::glyph({
            .out = &m_glyph,
            .glyph = m_barGlyphId.empty() ? "settings" : m_barGlyphId,
            .glyphSize = Style::baseGlyphSize * m_contentScale,
            .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
        })
    );
  }

  setRoot(std::move(area));
}

void SettingsWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  if (m_image != nullptr) {
    widget_custom_image::sync(
        *m_image, renderer, m_customImage, m_contentScale, widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface))
    );
    auto* node = root();
    if (node != nullptr) {
      node->setSize(m_image->width(), m_image->height());
    }
    return;
  }
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
