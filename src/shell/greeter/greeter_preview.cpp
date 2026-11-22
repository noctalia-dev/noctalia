#include "shell/greeter/greeter_preview.h"

#include "core/files/resource_paths.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "ui/builders.h"
#include "ui/controls/image.h"
#include "ui/palette.h"

#include <algorithm>
#include <memory>
#include <string_view>
#include <utility>

GreeterPreviewWidget::GreeterPreviewWidget(
    std::string component, float width, bool minimal, std::string userName, std::string displayName
)
    : m_component(std::move(component)), m_width(width), m_minimal(minimal), m_userName(std::move(userName)),
      m_displayName(std::move(displayName)) {}

void GreeterPreviewWidget::create() {
  auto scene = ui::node({});
  auto box = [&](Box*& out, ColorRole fill, float radius, bool border) {
    scene->addChild(
        ui::box(
            {.out = &out,
             .fill = colorSpecFromRole(fill),
             .border = colorSpecFromRole(ColorRole::OutlineVariant, border ? 1.0F : 0.0F),
             .borderWidth = border ? 1.0F : 0.0F,
             .styleRadius = radius}
        )
    );
  };
  box(m_panel, ColorRole::SurfaceContainer, 24, true);
  m_panel->setZIndex(-1);
  box(m_identity, ColorRole::Surface, 12, true);
  box(m_password, ColorRole::Surface, 12, true);
  box(m_session, ColorRole::OutlineVariant, 0, false);
  box(m_avatar, ColorRole::SecondaryContainer, 24, false);
  for (auto& label : m_labels) {
    scene->addChild(ui::label({.out = &label}));
  }
  scene->addChild(ui::glyph({.out = &m_symbol, .glyph = "moon-stars", .glyphSize = 22.0F}));
  scene->addChild(ui::glyph({.out = &m_userChevron, .glyph = "chevron-down", .glyphSize = 16.0F}));
  scene->addChild(ui::glyph({.out = &m_sessionChevron, .glyph = "chevron-down", .glyphSize = 16.0F}));
  scene->addChild(
      ui::button(
          {.out = &m_submit,
           .text = i18n::tr("greeter-editor.preview.sign-in"),
           .fontSize = 14.0F,
           .controlHeight = 48.0F,
           .variant = ButtonVariant::Primary}
      )
  );
  auto logo = std::make_unique<Image>();
  m_logo = logo.get();
  scene->addChild(std::move(logo));
  setRoot(std::move(scene));
}

void GreeterPreviewWidget::doLayout(Renderer& renderer) {
  const float s = contentScale();
  const auto position = [&](Node* node, float x, float y, float width, float height) {
    node->setVisible(true);
    node->setPosition(x * s, y * s);
    node->setSize(width * s, height * s);
  };
  const auto label = [&](std::size_t index, std::string_view value, float x, float y, float size,
                         FontWeight weight = FontWeight::Normal, ColorRole color = ColorRole::OnSurface) {
    auto* node = m_labels[index];
    node->setText(value);
    node->setFontSize(size * s);
    node->setFontWeight(weight);
    node->setColor(colorSpecFromRole(color));
    node->setMaxWidth(std::max(1.0F, m_width - x - (m_component == "login" ? 20 : 0)) * s);
    node->setFontFamily(m_fontFamily);
    node->measure(renderer);
    node->setPosition(x * s, y * s);
    node->setVisible(true);
  };
  for (auto* node : {m_panel, m_avatar, m_identity, m_password, m_session}) {
    node->setVisible(false);
  }
  for (auto* node : m_labels) {
    node->setVisible(false);
  }
  m_logo->setVisible(false);
  m_symbol->setVisible(false);
  m_userChevron->setVisible(false);
  m_sessionChevron->setVisible(false);
  m_submit->setVisible(false);
  if (m_component == "branding") {
    position(m_logo, 0, 0, 28, 28);
    m_logo->setSourceFile(renderer, paths::assetPath("noctalia.svg").string());
    root()->setSize(28 * s, 28 * s);
    return;
  }
  if (m_component != "login") {
    const auto key = "greeter-editor.preview." + m_component;
    const auto* glyph = m_component == "power" ? "power" : m_component == "keyboard" ? "keyboard" : "palette";
    position(m_symbol, 0, 8, 16, 16);
    m_symbol->setGlyph(glyph);
    m_symbol->setGlyphSize(16 * s);
    m_symbol->measure(renderer);
    m_labels[0]->setBaselineMode(LabelBaselineMode::InkCentered);
    label(0, i18n::tr(key), 26, 0, 14);
    const float rowHeight = 36 * s;
    m_symbol->setPosition(0, (rowHeight - m_symbol->height()) / 2);
    m_labels[0]->setPosition(26 * s, (rowHeight - m_labels[0]->height()) / 2);
    root()->setSize(m_width * s, rowHeight);
    return;
  }
  const float height = m_minimal ? 270.0F : 468.0F;
  const float pad = m_minimal ? 24.0F : 32.0F;
  const float inner = m_width - pad * 2;
  const float identityY = m_minimal ? 24.0F : 112.0F;
  position(m_panel, 0, 0, m_width, height);
  if (!m_minimal) {
    label(0, i18n::tr("greeter-editor.preview.welcome"), pad, 32, 25, FontWeight::Medium);
    label(1, i18n::tr("greeter-editor.preview.host"), pad, 76, 14, FontWeight::Normal, ColorRole::OnSurfaceVariant);
    position(m_identity, pad, identityY, inner, 64);
  }
  const float avatarX = pad + (m_minimal ? 0 : 13);
  position(m_avatar, avatarX, identityY + 12, 40, 40);
  position(m_symbol, avatarX + 9, identityY + 21, 22, 22);
  m_symbol->setGlyph("moon-stars");
  m_symbol->setGlyphSize(22 * s);
  m_symbol->measure(renderer);
  const bool showDisplayName = !m_minimal && m_displayName != m_userName;
  label(2, m_userName, pad + (m_minimal ? 54 : 66), identityY + (m_minimal ? 1 : 10), 18, FontWeight::Medium);
  if (m_minimal || showDisplayName) {
    label(
        3, m_minimal ? "nixos" : m_displayName, pad + (m_minimal ? 54 : 66), identityY + (m_minimal ? 32 : 36), 12,
        FontWeight::Normal, ColorRole::OnSurfaceVariant
    );
  } else {
    m_labels[2]->setPosition((pad + 66) * s, identityY * s + (64 * s - m_labels[2]->height()) / 2);
  }
  if (!m_minimal) {
    position(m_userChevron, m_width - pad - 30, identityY + 24, 16, 16);
    m_userChevron->setGlyphSize(16 * s);
    m_userChevron->measure(renderer);
  }
  const float promptY = m_minimal ? 102.0F : 200.0F;
  const float inputY = m_minimal ? 126.0F : 225.0F;
  label(5, i18n::tr("greeter-editor.preview.password"), pad, promptY, 13, FontWeight::Medium);
  position(m_password, pad, inputY, inner, m_minimal ? 44 : 48);
  label(
      6, i18n::tr("greeter-editor.preview.password-placeholder"), pad + 16, inputY + 14, 14, FontWeight::Normal,
      ColorRole::OnSurfaceVariant
  );
  if (!m_minimal) {
    position(m_session, pad, 324, inner, 1);
    m_session->setFill(colorSpecFromRole(ColorRole::OutlineVariant, 0.6F));
    label(
        7, i18n::tr("greeter-editor.preview.session"), pad + 16, 332, 13, FontWeight::Normal,
        ColorRole::OnSurfaceVariant
    );
    m_labels[7]->setPosition((pad + 16) * s, 332 * s + (40 * s - m_labels[7]->height()) / 2);
    label(8, "Umbriel", pad, 332, 13);
    m_labels[8]->setPosition(
        (m_width - pad - 44) * s - m_labels[8]->width(), 332 * s + (40 * s - m_labels[8]->height()) / 2
    );
    position(m_sessionChevron, m_width - pad - 32, 344, 16, 16);
    m_sessionChevron->setGlyphSize(16 * s);
    m_sessionChevron->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    m_sessionChevron->measure(renderer);
  }
  position(m_submit, pad, m_minimal ? 198.0F : 388.0F, inner, 48);
  m_submit->setControlHeight(48 * s);
  m_submit->setFontSize(14 * s);
  m_submit->setRadius(12 * s);
  m_submit->layout(renderer);
  root()->setSize(m_width * s, height * s);
}
