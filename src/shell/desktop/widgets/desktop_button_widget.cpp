#include "shell/desktop/widgets/desktop_button_widget.h"

#include "core/log.h"
#include "core/process.h"
#include "core/ui_phase.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace {

  constexpr Logger kLog("desktop-button");
  constexpr float kMinContentFit = 0.05f;
  constexpr float kMaxContentFit = 20.0f;

} // namespace

DesktopButtonWidget::DesktopButtonWidget(Options options)
    : m_glyph(std::move(options.glyph)), m_label(std::move(options.label)), m_command(std::move(options.command)),
      m_variant(options.variant), m_labelColor(options.labelColor), m_hoverBackground(options.hoverBackground),
      m_showBackground(options.showBackground) {}

void DesktopButtonWidget::create() {
  auto rootNode = std::make_unique<Node>();

  auto button = ui::button({
      .out = &m_button,
      .text = m_label,
      .glyph = m_glyph,
      .variant = m_variant,
      .onClick = [this]() { executeCommand(); },
  });
  if (m_animations != nullptr) {
    m_button->setAnimationManager(m_animations);
  }

  rootNode->addChild(std::move(button));
  setRoot(std::move(rootNode));
  syncButtonContent();
  syncButtonStyle();
  if (m_editorPreview) {
    setEditorPreview(true);
  }
}

void DesktopButtonWidget::setEditorPreview(bool enabled) noexcept {
  if (m_editorPreview == enabled) {
    return;
  }
  m_editorPreview = enabled;
  if (m_button == nullptr) {
    return;
  }
  if (InputArea* area = m_button->inputArea()) {
    area->setHitTestVisible(!enabled);
  }
  m_button->setHoverSuppressed(enabled);
}

void DesktopButtonWidget::layout(Renderer& renderer) {
  if (m_boxWidth <= 0.0f || m_boxHeight <= 0.0f) {
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
    const float contentWidth = std::max(1.0f, root()->width());
    if (contentWidth <= innerWidth + 0.5f) {
      break;
    }
    const float adjusted = fit * (innerWidth / contentWidth);
    if (std::abs(adjusted - fit) < 0.001f) {
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
      syncButtonContent();
      layout(renderer);
      return true;
    }
    return false;
  }
  if (key == "label") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_label = *v;
      syncButtonContent();
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

void DesktopButtonWidget::onFontFamilyChanged(const std::string& family, Renderer& /*renderer*/) {
  if (m_button != nullptr && m_button->label() != nullptr) {
    m_button->label()->setFontFamily(family);
  }
}

void DesktopButtonWidget::doLayout(Renderer& renderer) {
  if (m_button == nullptr || root() == nullptr) {
    return;
  }

  const bool boxed = m_boxWidth > 0.0f && m_boxHeight > 0.0f;
  const bool fillBox = boxed && m_fillToBox;
  const float innerW = boxed ? boxInnerWidth() : 0.0f;
  const float innerH = boxed ? boxInnerHeight() : 0.0f;

  const float scale = contentScale();
  m_button->setFontSize(Style::fontSizeBody * scale);
  m_button->setGlyphSize(Style::baseGlyphSize * scale);
  m_button->setPadding(Style::spaceSm * scale, Style::spaceMd * scale);
  m_button->setRadius(Style::scaledRadiusMd(scale));

  if (fillBox) {
    m_button->setMinWidth(innerW);
    m_button->setMinHeight(innerH);
  } else {
    // Glyph-only buttons pin a square explicit size; clear it so label changes can grow width.
    m_button->setSize(0.0f, 0.0f);
    m_button->setMinWidth(0.0f);
    m_button->setMinHeight(Style::controlHeight * scale);
  }

  if (!m_fontFamily.empty() && m_button->label() != nullptr) {
    m_button->label()->setFontFamily(m_fontFamily);
  }

  syncButtonContent();
  syncButtonStyle();

  m_button->layout(renderer);

  if (fillBox) {
    m_button->setSize(innerW, innerH);
  }

  m_button->updateInputArea();

  if (fillBox) {
    root()->setClipChildren(true);
    root()->setSize(innerW, innerH);
  } else {
    root()->setClipChildren(false);
    root()->setSize(m_button->width(), m_button->height());
  }
}

void DesktopButtonWidget::syncButtonContent() {
  if (m_button == nullptr) {
    return;
  }

  m_button->setText(m_label);
  if (m_glyph.empty()) {
    if (m_button->glyph() != nullptr) {
      m_button->glyph()->setVisible(false);
    }
  } else {
    m_button->setGlyph(m_glyph);
    if (m_button->glyph() != nullptr) {
      m_button->glyph()->setVisible(true);
    }
  }
}

void DesktopButtonWidget::syncButtonStyle() {
  if (m_button == nullptr) {
    return;
  }

  m_button->setVariant(m_variant);
  Button::ButtonPalette buttonPalette = Button::defaultPalette(m_variant);
  if (m_labelColor.has_value()) {
    buttonPalette.normal.label = *m_labelColor;
    buttonPalette.pressed.label = *m_labelColor;
    buttonPalette.disabled.label = *m_labelColor;
    if (buttonPalette.selected.has_value()) {
      buttonPalette.selected->label = *m_labelColor;
    }
  }
  buttonPalette.hover.bg = m_hoverBackground;

  if (!m_showBackground) {
    const auto stripChrome = [](const Button::ButtonStateColors& state) {
      return Button::ButtonStateColors{
          .bg = clearColorSpec(),
          .border = clearColorSpec(),
          .label = state.label,
      };
    };
    buttonPalette.borderWidth = 0.0f;
    buttonPalette.normal = stripChrome(buttonPalette.normal);
    buttonPalette.pressed = stripChrome(buttonPalette.pressed);
    buttonPalette.disabled = stripChrome(buttonPalette.disabled);
    if (buttonPalette.selected.has_value()) {
      buttonPalette.selected = stripChrome(*buttonPalette.selected);
    }
  }

  m_button->setCustomPalette(buttonPalette);
}

void DesktopButtonWidget::executeCommand() const {
  if (m_command.empty()) {
    return;
  }
  if (!process::runAsync(m_command)) {
    kLog.warn("failed to launch command");
  }
}
