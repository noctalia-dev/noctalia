#include "shell/desktop/widgets/desktop_button_widget.h"

#include "button_widget.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "system/process.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace {
  constexpr Logger kLog("desktop-button");
  constexpr float kMinContentFit = 0.05F;
  constexpr float kMaxContentFit = 20.0F;

} // namespace

DesktopButtonWidget::DesktopButtonWidget(Options options)
    : m_glyph(std::move(options.glyph)), m_label(std::move(options.label)), m_command(std::move(options.command)),
      m_variant(options.variant), m_labelColor(options.labelColor), m_hoverBackground(options.hoverBackground),
      m_showBackground(options.showBackground) {}

void DesktopButtonWidget::create() {
  auto button = std::make_unique<DesktopButton>();
  m_button = button.get();
  button->setAnimationManager(m_animations);
  button->setOnClick([this] { executeCommand(); });
  setRoot(std::move(button));
  syncButtonStyle();
  m_button->setEditorPreview(m_editorPreview);
}

void DesktopButtonWidget::setEditorPreview(bool enabled) noexcept {
  m_editorPreview = enabled;
  if (m_button != nullptr) {
    m_button->setEditorPreview(enabled);
  }
}

void DesktopButtonWidget::layout(Renderer& renderer) {
  if (m_boxWidth <= 0.0F || m_boxHeight <= 0.0F) {
    m_fillToBox = false;
    DesktopWidget::layout(renderer);
    return;
  }

  UiPhaseScope layoutPhase(UiPhase::Layout);
  onFontFamilyChanged(m_fontFamily, renderer);

  const float innerWidth = boxInnerWidth();
  const float innerHeight = boxInnerHeight();
  const float referenceHeight = Style::controlHeight * m_baseScale;

  // Height drives the primary scale; clamp down when a long label would exceed the box width.
  float fit = std::clamp(innerHeight / referenceHeight, kMinContentFit, kMaxContentFit);
  for (int attempt = 0; attempt < 4; ++attempt) {
    m_fillToBox = false;
    m_contentScale = m_baseScale * fit;
    doLayout(renderer);
    const float contentWidth = std::max(1.0F, root()->width());
    if (contentWidth <= innerWidth + 0.5F) {
      break;
    }
    const float adjusted = fit * (innerWidth / contentWidth);
    if (std::abs(adjusted - fit) < 0.001F) {
      break;
    }
    fit = std::clamp(adjusted, kMinContentFit, kMaxContentFit);
  }

  m_fillToBox = true;
  m_contentScale = m_baseScale * fit;
  doLayout(renderer);
  applyBackground();
}

bool DesktopButtonWidget::applySetting(
    const std::string& key, const WidgetSettingValue& value,
    const std::unordered_map<std::string, WidgetSettingValue>& /*allSettings*/, Renderer& renderer
) {
  if (m_button == nullptr) {
    return false;
  }

  if (key == "glyph") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_glyph = *v;
      syncButtonStyle();
      layout(renderer);
      return true;
    }
    return false;
  }
  if (key == "label") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_label = *v;
      syncButtonStyle();
      layout(renderer);
      return true;
    }
    return false;
  }
  if (key == "command") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_command = *v;
      return true;
    }
    return false;
  }
  if (key == "background") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_showBackground = *v;
      syncButtonStyle();
      requestRedraw();
      return true;
    }
    return false;
  }
  if (key == "variant") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      if (*v == "default") {
        m_variant = ButtonVariant::Default;
      } else if (*v == "primary") {
        m_variant = ButtonVariant::Primary;
      } else if (*v == "secondary") {
        m_variant = ButtonVariant::Secondary;
      } else if (*v == "destructive") {
        m_variant = ButtonVariant::Destructive;
      } else if (*v == "outline") {
        m_variant = ButtonVariant::Outline;
      } else if (*v == "ghost") {
        m_variant = ButtonVariant::Ghost;
      } else {
        return false;
      }
      syncButtonStyle();
      layout(renderer);
      return true;
    }
    return false;
  }
  if (key == "hover_background") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_hoverBackground = colorSpecFromConfigString(*v, key);
      syncButtonStyle();
      requestRedraw();
      return true;
    }
    return false;
  }
  if (key == "color") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      if (v->empty()) {
        m_labelColor.reset();
      } else {
        m_labelColor = colorSpecFromConfigString(*v, key);
      }
      syncButtonStyle();
      requestRedraw();
      return true;
    }
    return false;
  }

  return DesktopWidget::applySetting(key, value, {}, renderer);
}

void DesktopButtonWidget::onFontFamilyChanged(const std::string& family, Renderer&) {
  if (m_button != nullptr) {
    m_button->setFontFamily(family);
  }
}

void DesktopButtonWidget::doLayout(Renderer& renderer) {
  if (m_button == nullptr) {
    return;
  }
  m_button->setContentScale(contentScale());
  m_button->setFontFamily(m_fontFamily);
  m_button->setFillSize(m_fillToBox ? boxInnerWidth() : 0.0F, m_fillToBox ? boxInnerHeight() : 0.0F);
  syncButtonStyle();
  m_button->layout(renderer);
}

void DesktopButtonWidget::syncButtonStyle() {
  if (m_button != nullptr) {
    m_button->setOptions(
        {.glyph = m_glyph,
         .label = m_label,
         .variant = m_variant,
         .showBackground = m_showBackground,
         .labelColor = m_labelColor,
         .hoverBackground = m_hoverBackground}
    );
  }
}

void DesktopButtonWidget::executeCommand() const {
  if (m_command.empty()) {
    return;
  }
  if (!process::runAsync(m_command)) {
    kLog.warn("failed to launch command");
  }
}
