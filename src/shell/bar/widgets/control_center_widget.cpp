#include "shell/bar/widgets/control_center_widget.h"

#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>

ControlCenterWidget::ControlCenterWidget(wl_output* /*output*/, std::string barGlyphId, std::string logoPath)
    : m_barGlyphId(std::move(barGlyphId)), m_logoPath(std::move(logoPath)) {}

void ControlCenterWidget::create() {
  auto area = std::make_unique<InputArea>();
  area->setOnClick([this](const InputArea::PointerData& /*data*/) { requestPanelToggle("control-center", "home"); });

  if (!m_logoPath.empty()) {
    area->addChild(
        ui::image({
            .out = &m_image,
            .fit = ImageFit::Contain,
        })
    );
  } else {
    area->addChild(
        ui::glyph({
            .out = &m_glyph,
            .glyph = m_barGlyphId.empty() ? "search" : m_barGlyphId,
            .glyphSize = Style::barGlyphSize * m_contentScale,
            .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
        })
    );
  }

  setRoot(std::move(area));
}

void ControlCenterWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  auto* node = root();
  if (node == nullptr) {
    return;
  }

  if (m_image != nullptr) {
    m_image->setSize(Style::barIconSize * m_contentScale, Style::barIconSize * m_contentScale);
    const int logoTargetSize = std::max(1, static_cast<int>(48.0f * m_contentScale));
    m_image->setSourceFile(renderer, m_logoPath, logoTargetSize, true);
    node->setSize(m_image->width(), m_image->height());
  } else if (m_glyph != nullptr) {
    m_glyph->setGlyphSize(Style::barGlyphSize * m_contentScale);
    m_glyph->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
    m_glyph->measure(renderer);
    node->setSize(m_glyph->width(), m_glyph->height());
  }
}
