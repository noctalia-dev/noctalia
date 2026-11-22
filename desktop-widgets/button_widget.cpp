#include "button_widget.h"

#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/style.h"

#include <utility>

DesktopButton::DesktopButton() {
  auto button = ui::button({
      .out = &m_button,
      .text = m_label,
      .glyph = m_glyph,
      .variant = m_variant,

  });
  addChild(std::move(button));
}

void DesktopButton::setOptions(const Options& options) {
  m_glyph = options.glyph;
  m_label = options.label;
  m_variant = options.variant;
  m_showBackground = options.showBackground;
  m_labelColor = options.labelColor;
  m_hoverBackground = options.hoverBackground;
  syncButtonContent();
  syncButtonStyle();
}

void DesktopButton::setOnClick(std::function<void()> callback) { m_button->setOnClick(std::move(callback)); }

void DesktopButton::setTabStop(bool tabStop) { m_button->setTabStop(tabStop); }

void DesktopButton::setEditorPreview(bool enabled) {
  m_button->inputArea()->setHitTestVisible(!enabled);
  m_button->setHoverSuppressed(enabled);
}

void DesktopButton::setContentScale(float scale) {
  if (m_contentScale == scale) {
    return;
  }
  m_contentScale = scale;
  markLayoutDirty();
}

void DesktopButton::setFontFamily(const std::string& family) {
  m_fontFamily = family;
  if (m_button->label() != nullptr) {
    m_button->label()->setFontFamily(family);
  }
}

void DesktopButton::setFillSize(float width, float height) {
  if (m_fillWidth == width && m_fillHeight == height) {
    return;
  }
  m_fillWidth = width;
  m_fillHeight = height;
  markLayoutDirty();
}

void DesktopButton::doLayout(Renderer& renderer) {
  if (m_button == nullptr) {
    return;
  }

  const bool fillBox = m_fillWidth > 0.0F && m_fillHeight > 0.0F;
  const float innerW = m_fillWidth;
  const float innerH = m_fillHeight;

  const float scale = m_contentScale;
  m_button->setFontSize(Style::fontSizeBody * scale);
  m_button->setGlyphSize(Style::baseGlyphSize * scale);
  m_button->setPadding(Style::spaceSm * scale, Style::spaceMd * scale);
  m_button->setStyleRadius(Style::radiusMd, scale);

  if (fillBox) {
    m_button->setMinWidth(innerW);
    m_button->setMinHeight(innerH);
  } else {
    // Glyph-only buttons pin a square explicit size; clear it so label changes can grow width.
    m_button->setSize(0.0F, 0.0F);
    m_button->setMinWidth(0.0F);
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
    setClipChildren(true);
    setSize(innerW, innerH);
  } else {
    setClipChildren(false);
    setSize(m_button->width(), m_button->height());
  }
}

void DesktopButton::syncButtonContent() {
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

void DesktopButton::syncButtonStyle() {
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
    buttonPalette.borderWidth = 0.0F;
    buttonPalette.normal = stripChrome(buttonPalette.normal);
    buttonPalette.pressed = stripChrome(buttonPalette.pressed);
    buttonPalette.disabled = stripChrome(buttonPalette.disabled);
    if (buttonPalette.selected.has_value()) {
      buttonPalette.selected = stripChrome(*buttonPalette.selected);
    }
  }

  m_button->setCustomPalette(buttonPalette);
}
