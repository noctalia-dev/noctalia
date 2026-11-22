#include "label_widget.h"

#include "core/ui_phase.h"
#include "ui/builders.h"
#include "ui/controls/label.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace {
  constexpr float kMaxWidth = 320.0F;
  constexpr float kShadowAlpha = 0.6F;
  constexpr float kShadowOffset = 1.5F;
} // namespace

LabelWidget::LabelWidget() {
  setHitTestVisible(false);
  addChild(
      ui::label({
          .out = &m_title,
          .text = "Title",
          .fontWeight = FontWeight::Bold,
          .maxLines = 3,
          .textAlign = TextAlign::Start,
      })
  );
  addChild(ui::label({.out = &m_description, .maxLines = 0, .textAlign = TextAlign::Start, .visible = false}));
  applyShadow();
}

void LabelWidget::setTitle(std::string_view title) {
  uiAssertNotRendering("LabelWidget::setTitle");
  m_title->setText(title);
}

void LabelWidget::setDescription(std::string_view description) {
  uiAssertNotRendering("LabelWidget::setDescription");
  m_description->setText(description);
  m_description->setVisible(!description.empty());
}

void LabelWidget::setFontFamily(std::string family) {
  uiAssertNotRendering("LabelWidget::setFontFamily");
  m_title->setFontFamily(family);
  m_description->setFontFamily(std::move(family));
}

void LabelWidget::setColor(const ColorSpec& color) {
  uiAssertNotRendering("LabelWidget::setColor");
  if (m_color == color) {
    return;
  }
  m_color = color;
  m_title->setColor(color);
  m_description->setColor(color);
}

void LabelWidget::setShadow(bool enabled) {
  uiAssertNotRendering("LabelWidget::setShadow");
  if (m_shadow == enabled) {
    return;
  }
  m_shadow = enabled;
  applyShadow();
  markLayoutDirty();
}

void LabelWidget::setContentScale(float scale) {
  uiAssertNotRendering("LabelWidget::setContentScale");
  if (!std::isfinite(scale) || scale <= 0.0F) {
    throw std::invalid_argument("Label content scale must be positive and finite");
  }
  if (m_contentScale == scale) {
    return;
  }
  m_contentScale = scale;
  markLayoutDirty();
}

const std::string& LabelWidget::title() const noexcept { return m_title->text(); }
const std::string& LabelWidget::description() const noexcept { return m_description->text(); }
const std::string& LabelWidget::fontFamily() const noexcept { return m_title->font().family; }

void LabelWidget::applyShadow() {
  const float offset = kShadowOffset * m_contentScale;
  const auto color = colorSpecFromRole(ColorRole::Shadow, kShadowAlpha);
  for (auto* label : {m_title, m_description}) {
    if (m_shadow) {
      label->setShadow(color, offset, offset);
    } else {
      label->clearShadow();
    }
  }
}

void LabelWidget::doLayout(Renderer& renderer) {
  UiPhaseScope phase(UiPhase::Layout);
  const float maxWidth = std::max(1.0F, kMaxWidth * m_contentScale);
  m_title->setFontSize(Style::fontSizeBody * 1.5F * m_contentScale);
  m_title->setMaxWidth(maxWidth);
  m_title->measure(renderer);
  m_title->setPosition(0.0F, 0.0F);
  float width = m_title->width();
  float height = m_title->height();
  if (m_description->visible()) {
    m_description->setFontSize(Style::fontSizeBody * m_contentScale);
    m_description->setMaxWidth(maxWidth);
    m_description->measure(renderer);
    m_description->setPosition(0.0F, height);
    width = std::max(width, m_description->width());
    height += m_description->height();
  }
  const float shadowPad = m_shadow ? kShadowOffset * m_contentScale : 0.0F;
  setSize(width + shadowPad, height + shadowPad);
  applyShadow();
}
