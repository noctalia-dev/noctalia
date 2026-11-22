#include "shell/desktop/widgets/desktop_label_widget.h"

#include "label_widget.h"

#include <algorithm>
#include <memory>
#include <utility>

DesktopLabelWidget::DesktopLabelWidget(Options options)
    : m_title(std::move(options.title)), m_description(std::move(options.description)), m_color(options.color),
      m_opacity(std::clamp(options.opacity, 0.0F, 1.0F)), m_shadow(options.shadow) {}

void DesktopLabelWidget::create() {
  auto label = std::make_unique<LabelWidget>();
  m_label = label.get();
  label->setTitle(m_title);
  label->setDescription(m_description);
  label->setColor(m_color);
  label->setOpacity(m_opacity);
  label->setShadow(m_shadow);
  label->setContentScale(contentScale());
  setRoot(std::move(label));
}

bool DesktopLabelWidget::applySetting(
    const std::string& key, const WidgetSettingValue& value,
    const std::unordered_map<std::string, WidgetSettingValue>& /*allSettings*/, Renderer& renderer
) {
  if (key == "title") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_title = *v;
      if (m_label != nullptr) {
        m_label->setTitle(m_title);
      }
      requestLayout();
      return true;
    }
    return false;
  }
  if (key == "description") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_description = *v;
      if (m_label != nullptr) {
        m_label->setDescription(m_description);
      }
      requestLayout();
      return true;
    }
    return false;
  }
  if (key == "color") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_color = colorSpecFromConfigString(*v, key);
      if (m_label != nullptr) {
        m_label->setColor(m_color);
      }
      requestLayout();
      return true;
    }
    return false;
  }
  if (key == "opacity") {
    if (const auto* v = std::get_if<double>(&value)) {
      m_opacity = std::clamp(static_cast<float>(*v), 0.0F, 1.0F);
      if (m_label != nullptr) {
        m_label->setOpacity(m_opacity);
      }
      return true;
    }
    if (const auto* v = std::get_if<std::int64_t>(&value)) {
      m_opacity = std::clamp(static_cast<float>(*v), 0.0F, 1.0F);
      if (m_label != nullptr) {
        m_label->setOpacity(m_opacity);
      }
      return true;
    }
    return false;
  }
  if (key == "shadow") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_shadow = *v;
      if (m_label != nullptr) {
        m_label->setShadow(m_shadow);
      }
      requestLayout();
      return true;
    }
    return false;
  }
  return DesktopWidget::applySetting(key, value, {}, renderer);
}

void DesktopLabelWidget::onFontFamilyChanged(const std::string& family, Renderer& /*renderer*/) {
  if (m_label != nullptr) {
    m_label->setFontFamily(family);
  }
}

void DesktopLabelWidget::doLayout(Renderer& renderer) {
  if (m_label != nullptr) {
    m_label->setContentScale(contentScale());
    m_label->layout(renderer);
  }
}
