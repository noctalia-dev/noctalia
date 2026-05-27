#include "shell/desktop/widgets/desktop_clock_widget.h"

#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <memory>

namespace {

  bool formatShowsSeconds(const std::string& format) {
    return format.find("%S") != std::string::npos
        || format.find("%T") != std::string::npos
        || format.find("%X") != std::string::npos;
  }

  float clockFontSize(float contentScale) { return Style::fontSizeBody * 4.0f * contentScale; }

} // namespace

namespace {

  constexpr float kShadowAlpha = 0.6f;
  constexpr float kShadowOffset = 1.5f;

} // namespace

DesktopClockWidget::DesktopClockWidget(std::string format, ColorSpec color, bool shadow)
    : m_format(std::move(format)), m_color(color), m_shadow(shadow), m_showsSeconds(formatShowsSeconds(m_format)) {}

void DesktopClockWidget::create() {
  auto rootNode = std::make_unique<Node>();

  auto label = ui::label({
      .out = &m_label,
      .fontSize = clockFontSize(contentScale()),
      .color = m_color,
      .fontWeight = FontWeight::Bold,
      .textAlign = TextAlign::Center,
  });
  rootNode->addChild(std::move(label));
  setRoot(std::move(rootNode));
  applyShadow();
}

bool DesktopClockWidget::wantsSecondTicks() const { return m_showsSeconds; }

std::string DesktopClockWidget::formatText() const { return formatLocalTime(m_format.c_str()); }

bool DesktopClockWidget::applySetting(
    const std::string& key, const WidgetSettingValue& value,
    const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
) {
  if (key == "color") {
    if (const auto* v = std::get_if<std::string>(&value); v != nullptr && m_label != nullptr) {
      m_color = colorSpecFromConfigString(*v, key);
      m_label->setColor(m_color);
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
  return DesktopWidget::applySetting(key, value, allSettings, renderer);
}

void DesktopClockWidget::doLayout(Renderer& renderer) {
  if (m_label == nullptr || root() == nullptr) {
    return;
  }

  m_label->setFontSize(clockFontSize(contentScale()));
  applyShadow();
  update(renderer);
  m_label->measure(renderer);
  m_label->setPosition(0.0f, 0.0f);
  root()->setSize(m_label->width(), m_label->height());
}

void DesktopClockWidget::doUpdate(Renderer& renderer) {
  if (m_label == nullptr) {
    return;
  }

  m_label->setFontSize(clockFontSize(contentScale()));
  const std::string text = formatText();
  if (text == m_lastText) {
    return;
  }

  m_lastText = text;
  m_label->setText(m_lastText);
  m_label->measure(renderer);
}

void DesktopClockWidget::applyShadow() {
  if (m_label == nullptr) {
    return;
  }
  if (m_shadow) {
    const float offset = kShadowOffset * contentScale();
    m_label->setShadow(Color(0.0f, 0.0f, 0.0f, kShadowAlpha), offset, offset);
  } else {
    m_label->clearShadow();
  }
}
