#include "shell/desktop/widgets/desktop_login_box_widget.h"

#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/lockscreen/lockscreen_login_box.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <memory>

namespace {

  constexpr float kLoginGlyphSize = 16.0f;

  [[nodiscard]] bool isStyleSetting(std::string_view key) {
    return key == "background_color"
        || key == "background_opacity"
        || key == "background_radius"
        || key == lockscreen_login_box::kShowLoginButtonKey
        || key == lockscreen_login_box::kInputOpacityKey
        || key == lockscreen_login_box::kInputRadiusKey;
  }

} // namespace

void DesktopLoginBoxWidget::create() {
  auto rootNode = std::make_unique<Node>();

  auto panel = ui::box({});
  m_panel = panel.get();
  rootNode->addChild(std::move(panel));

  auto passwordGhost = ui::box({});
  m_passwordGhost = passwordGhost.get();
  rootNode->addChild(std::move(passwordGhost));

  auto loginButtonGhost = ui::box({
      .fill = colorSpecFromRole(ColorRole::Primary, 0.9f),
  });
  m_loginButtonGhost = loginButtonGhost.get();
  rootNode->addChild(std::move(loginButtonGhost));

  auto loginGlyph = ui::glyph({
      .out = &m_loginGlyph,
      .glyph = "check",
      .glyphSize = kLoginGlyphSize,
      .color = colorSpecFromRole(ColorRole::OnPrimary),
  });
  rootNode->addChild(std::move(loginGlyph));

  setRoot(std::move(rootNode));
}

void DesktopLoginBoxWidget::setSettings(const std::unordered_map<std::string, WidgetSettingValue>& settings) {
  m_settings = settings;
  lockscreen_login_box::normalizeSettings(m_settings);
}

bool DesktopLoginBoxWidget::applySetting(
    const std::string& key, const WidgetSettingValue& value,
    const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
) {
  (void)value;
  m_settings = allSettings;
  lockscreen_login_box::normalizeSettings(m_settings);
  if (!isStyleSetting(key)) {
    return false;
  }
  doLayout(renderer);
  return true;
}

void DesktopLoginBoxWidget::doLayout(Renderer& renderer) {
  const float screenWidth = m_screenWidth > 0.0f ? m_screenWidth : 1920.0f;
  const float panelWidth = lockscreen_login_box::resolvePanelWidth(screenWidth, m_boxWidth);
  const float panelHeight = lockscreen_login_box::resolvePanelHeight(m_boxHeight);
  const lockscreen_login_box::LoginBoxStyle style = lockscreen_login_box::resolveStyle(m_settings);
  const lockscreen_login_box::PanelContentLayout layout =
      lockscreen_login_box::panelContentLayout(panelWidth, panelHeight, style.showLoginButton);

  if (m_panel != nullptr) {
    m_panel->setPosition(0.0f, 0.0f);
    m_panel->setSize(panelWidth, panelHeight);
    m_panel->setStyle(
        RoundedRectStyle{
            .fill = resolveColorSpec(style.panelFill),
            .border = colorForRole(ColorRole::Outline),
            .fillMode = FillMode::Solid,
            .radius = Style::scaledRadius(style.panelRadius),
            .softness = 1.0f,
            .borderWidth = Style::borderWidth,
        }
    );
  }

  if (m_passwordGhost != nullptr) {
    m_passwordGhost->setPosition(layout.contentLeft, layout.contentTop);
    m_passwordGhost->setSize(layout.inputWidth, layout.controlHeight);
    m_passwordGhost->setStyle(
        RoundedRectStyle{
            .fill = colorForRole(ColorRole::Surface, style.inputOpacity),
            .border = colorForRole(ColorRole::Outline),
            .fillMode = FillMode::Solid,
            .radius = Style::scaledRadius(style.inputRadius),
            .borderWidth = Style::borderWidth,
        }
    );
  }

  if (m_loginButtonGhost != nullptr) {
    m_loginButtonGhost->setVisible(style.showLoginButton);
    if (style.showLoginButton) {
      m_loginButtonGhost->setPosition(layout.buttonX, layout.contentTop);
      m_loginButtonGhost->setSize(layout.controlHeight, layout.controlHeight);
      m_loginButtonGhost->setStyle(
          RoundedRectStyle{
              .fill = colorForRole(ColorRole::Primary, 0.9f),
              .fillMode = FillMode::Solid,
              .radius = Style::scaledRadius(style.inputRadius),
          }
      );
    }
  }

  if (m_loginGlyph != nullptr) {
    m_loginGlyph->setVisible(style.showLoginButton);
    if (style.showLoginButton) {
      m_loginGlyph->setPosition(
          layout.buttonX + (layout.controlHeight - kLoginGlyphSize) * 0.5f,
          layout.contentTop + (layout.controlHeight - kLoginGlyphSize) * 0.5f
      );
      m_loginGlyph->measure(renderer);
    }
  }

  if (Node* rootNode = root()) {
    rootNode->setSize(panelWidth, panelHeight);
  }
}
