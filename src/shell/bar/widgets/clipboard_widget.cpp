#include "shell/bar/widgets/clipboard_widget.h"

#include "render/scene/input_area.h"
#include "shell/wayland/clipboard_service.h"
#include "ui/builders.h"
#include "ui/controls/image.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>

namespace {

  // Only the start of a copy fits in the bar, so this bounds how much of it is collapsed to one line.
  constexpr std::size_t kLabelSourceBytes = 512;

} // namespace

ClipboardWidget::ClipboardWidget(const ClipboardService* clipboard, wl_output* /*output*/, Options options)
    : m_clipboard(clipboard), m_barGlyphId(std::move(options.glyph)),
      m_customImage(widget_custom_image::fromConfig(options.customImage, options.customImageColorize)),
      m_showLabel(options.showLabel), m_maxWidth(static_cast<float>(options.maxWidth)),
      m_glyphPosition(options.glyphPosition) {
  setPeek(options.peek, options.peekDelayMs);
}

void ClipboardWidget::create() {
  auto area = ui::inputArea({});

  if (m_customImage.enabled()) {
    area->addChild(ui::image({.out = &m_image, .fit = ImageFit::Contain}));
  } else {
    area->addChild(
        ui::glyph({
            .out = &m_glyph,
            .glyph = m_barGlyphId,
            .glyphSize = Style::baseGlyphSize * m_contentScale,
            .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
        })
    );
  }

  if (m_showLabel) {
    area->addChild(
        ui::label({
            .out = &m_label,
            .fontSize = Style::fontSizeBody * fontScale(),
            .fontWeight = labelFontWeight(),
            .fontFamily = labelFontFamily(),
            .maxLines = 1,
        })
    );
  }

  setRoot(std::move(area));
}

void ClipboardWidget::doUpdate(Renderer& /*renderer*/) {
  if (m_label != nullptr) {
    syncLabel();
  }
}

void ClipboardWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  if (root() == nullptr) {
    return;
  }
  const bool isVertical = containerHeight > containerWidth;

  Node* icon = nullptr;
  if (m_image != nullptr) {
    widget_custom_image::sync(
        *m_image, renderer, m_customImage, m_contentScale, widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface))
    );
    icon = m_image;
  } else if (m_glyph != nullptr) {
    m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
    m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
    m_glyph->measure(renderer);
    icon = m_glyph;
  }

  if (m_label != nullptr) {
    const float iconWidth = icon != nullptr && icon->visible() ? icon->width() : 0.0F;
    const float gap = iconWidth > 0.0F ? Style::spaceXs : 0.0F;
    // Clipboard text does not fit across a vertical bar, so there the widget shows its icon only.
    m_label->setVisible(!isVertical && !m_labelText.empty());
    m_label->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
    m_label->setMaxWidth(std::max(0.0F, m_maxWidth * m_contentScale - iconWidth - gap));
    m_label->measure(renderer);
  }

  layoutGlyphValue({
      .glyph = icon,
      .value = m_label,
      .glyphPosition = m_glyphPosition,
      .vertical = isVertical,
      .horizontalGap = Style::spaceXs,
  });
}

void ClipboardWidget::syncLabel() {
  if (m_clipboard == nullptr || m_clipboard->selectionSerial() == m_labelSelectionSerial) {
    return;
  }
  m_labelSelectionSerial = m_clipboard->selectionSerial();

  const std::string_view text = m_clipboard->clipboardDisplayText();
  std::string labelText =
      StringUtils::windowTitleSingleLine(StringUtils::truncateUtf8(StringUtils::trimLeftView(text), kLabelSourceBytes));
  if (labelText == m_labelText) {
    return;
  }
  m_labelText = std::move(labelText);
  m_label->setText(m_labelText);

  auto* area = static_cast<InputArea*>(root());
  if (m_labelText.empty()) {
    area->clearTooltip();
  } else {
    area->setTooltip(m_labelText);
  }
}
