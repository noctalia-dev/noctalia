#include "shell/desktop/widgets/desktop_label_widget.h"

#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace {

  constexpr float kBaseMaxWidth = 320.0f;
  constexpr float kShadowAlpha = 0.6f;
  constexpr float kShadowOffset = 1.5f;

  float titleFontSize(float contentScale) { return Style::fontSizeBody * 1.5f * contentScale; }
  float descriptionFontSize(float contentScale) { return Style::fontSizeBody * contentScale; }

} // namespace

DesktopLabelWidget::DesktopLabelWidget(Options options)
    : m_title(std::move(options.title)), m_description(std::move(options.description)), m_color(options.color),
      m_opacity(std::clamp(options.opacity, 0.0f, 1.0f)), m_shadow(options.shadow) {}

void DesktopLabelWidget::create() {
  auto rootNode = std::make_unique<Node>();
  rootNode->setClipChildren(true);

  auto titleLabel = ui::label({
      .out = &m_titleLabel,
      .text = m_title,
      .fontSize = titleFontSize(contentScale()),
      .color = m_color,
      .maxLines = 3,
      .fontWeight = FontWeight::Bold,
      .textAlign = TextAlign::Start,
  });
  rootNode->addChild(std::move(titleLabel));

  auto descriptionLabel = ui::label({
      .out = &m_descriptionLabel,
      .text = m_description,
      .fontSize = descriptionFontSize(contentScale()),
      .color = m_color,
      .maxLines = 0,
      .textAlign = TextAlign::Start,
  });
  rootNode->addChild(std::move(descriptionLabel));

  setRoot(std::move(rootNode));
  applyLabelColors();
  applyShadow();
}

bool DesktopLabelWidget::applySetting(
    const std::string& key, const WidgetSettingValue& value,
    const std::unordered_map<std::string, WidgetSettingValue>& /*allSettings*/, Renderer& renderer
) {
  if (key == "title") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_title = *v;
      if (m_titleLabel != nullptr) {
        m_titleLabel->setText(m_title);
      }
      requestLayout();
      return true;
    }
    return false;
  }
  if (key == "description") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_description = *v;
      if (m_descriptionLabel != nullptr) {
        m_descriptionLabel->setText(m_description);
      }
      requestLayout();
      return true;
    }
    return false;
  }
  if (key == "color") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_color = colorSpecFromConfigString(*v, key);
      applyLabelColors();
      requestLayout();
      return true;
    }
    return false;
  }
  if (key == "opacity") {
    if (const auto* v = std::get_if<double>(&value)) {
      m_opacity = std::clamp(static_cast<float>(*v), 0.0f, 1.0f);
      applyLabelColors();
      applyShadow();
      return true;
    }
    if (const auto* v = std::get_if<std::int64_t>(&value)) {
      m_opacity = std::clamp(static_cast<float>(*v), 0.0f, 1.0f);
      applyLabelColors();
      applyShadow();
      return true;
    }
    return false;
  }
  if (key == "shadow") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_shadow = *v;
      applyShadow();
      return true;
    }
    return false;
  }
  return DesktopWidget::applySetting(key, value, {}, renderer);
}

void DesktopLabelWidget::onFontFamilyChanged(const std::string& family, Renderer& /*renderer*/) {
  if (m_titleLabel != nullptr) {
    m_titleLabel->setFontFamily(family);
  }
  if (m_descriptionLabel != nullptr) {
    m_descriptionLabel->setFontFamily(family);
  }
}

void DesktopLabelWidget::doLayout(Renderer& renderer) {
  if (root() == nullptr || m_titleLabel == nullptr || m_descriptionLabel == nullptr) {
    return;
  }

  const float scale = contentScale();
  const float maxWidth = std::max(1.0f, kBaseMaxWidth * scale);
  const bool showDescription = !m_description.empty();

  m_titleLabel->setFontSize(titleFontSize(scale));
  m_titleLabel->setMaxWidth(maxWidth);
  m_titleLabel->setText(m_title);
  m_titleLabel->measure(renderer);

  float height = m_titleLabel->height();
  float width = m_titleLabel->width();

  m_descriptionLabel->setVisible(showDescription);
  if (showDescription) {
    m_descriptionLabel->setFontSize(descriptionFontSize(scale));
    m_descriptionLabel->setMaxWidth(maxWidth);
    m_descriptionLabel->setText(m_description);
    m_descriptionLabel->measure(renderer);
    m_descriptionLabel->setPosition(0.0f, height);
    height += m_descriptionLabel->height();
    width = std::max(width, m_descriptionLabel->width());
  }

  m_titleLabel->setPosition(0.0f, 0.0f);
  root()->setSize(width, height);
  applyLabelColors();
  applyShadow();
}

void DesktopLabelWidget::applyLabelColors() {
  ColorSpec effective = m_color;
  effective.alpha *= m_opacity;
  if (m_titleLabel != nullptr) {
    m_titleLabel->setColor(effective);
  }
  if (m_descriptionLabel != nullptr) {
    m_descriptionLabel->setColor(effective);
  }
}

void DesktopLabelWidget::applyShadow() {
  const float offset = kShadowOffset * contentScale();
  const Color shadowColor(0.0f, 0.0f, 0.0f, kShadowAlpha * m_opacity);

  auto applyTo = [this, offset, shadowColor](Label* label) {
    if (label == nullptr) {
      return;
    }
    if (m_shadow) {
      label->setShadow(shadowColor, offset, offset);
    } else {
      label->clearShadow();
    }
  };

  applyTo(m_titleLabel);
  applyTo(m_descriptionLabel);
}
