#include "shell/desktop/widgets/desktop_login_box_widget.h"

#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/lockscreen/lockscreen_login_box.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>
#include <string_view>

namespace {

  constexpr float kLoginGlyphSize = 16.0F;

  [[nodiscard]] bool isStyleSetting(std::string_view key) {
    return key == "background_color"
        || key == "background_opacity"
        || key == "background_radius"
        || key == lockscreen_login_box::kLayoutKey
        || key == lockscreen_login_box::kShowSessionButtonsKey
        || key == lockscreen_login_box::kShowMediaKey
        || key == lockscreen_login_box::kShowWeatherKey
        || key == lockscreen_login_box::kShowLoginButtonKey
        || key == lockscreen_login_box::kShowCapsLockKey
        || key == lockscreen_login_box::kShowUnlockHintKey
        || key == lockscreen_login_box::kInputOpacityKey
        || key == lockscreen_login_box::kInputRadiusKey;
  }

} // namespace

void DesktopLoginBoxWidget::create() {
  auto rootNode = ui::node({});

  auto panel = ui::box({});
  m_panel = panel.get();
  rootNode->addChild(std::move(panel));

  auto infoGhost = ui::box({});
  m_infoGhost = infoGhost.get();
  rootNode->addChild(std::move(infoGhost));

  auto mediaGhost = ui::box({});
  m_mediaGhost = mediaGhost.get();
  rootNode->addChild(std::move(mediaGhost));

  auto weatherGhost = ui::box({});
  m_weatherGhost = weatherGhost.get();
  rootNode->addChild(std::move(weatherGhost));

  auto statusGhost = ui::box({});
  m_statusGhost = statusGhost.get();
  rootNode->addChild(std::move(statusGhost));

  auto passwordGhost = ui::box({});
  m_passwordGhost = passwordGhost.get();
  rootNode->addChild(std::move(passwordGhost));

  auto loginButtonGhost = ui::box({
      .fill = colorSpecFromRole(ColorRole::Primary, 0.9F),
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

  for (Box*& sessionSlot : m_sessionGhosts) {
    auto sessionGhost = ui::box({});
    sessionSlot = sessionGhost.get();
    rootNode->addChild(std::move(sessionGhost));
  }

  setRoot(std::move(rootNode));
}

void DesktopLoginBoxWidget::layout(Renderer& renderer) {
  DesktopWidget::layout(renderer);
  // Unlock-hint ghost sits above/below the panel (outside the box); don't clip it away.
  if (Node* outer = presentationRoot()) {
    outer->setClipChildren(false);
  }
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
  if (Node* outer = presentationRoot()) {
    outer->setClipChildren(false);
  }
  return true;
}

void DesktopLoginBoxWidget::doLayout(Renderer& renderer) {
  const float screenWidth = m_screenWidth > 0.0F ? m_screenWidth : 1920.0F;
  const float screenHeight = m_screenHeight > 0.0F ? m_screenHeight : 1080.0F;
  const lockscreen_login_box::LoginBoxStyle style = lockscreen_login_box::resolveStyle(m_settings);
  const float panelWidth = lockscreen_login_box::resolvePanelWidth(screenWidth, m_boxWidth, style.layout);
  const bool showInfo = lockscreen_login_box::styleShowsInfoExtras(style);
  const float panelHeight = lockscreen_login_box::defaultPanelHeight(style.layout, style.showSessionButtons, showInfo);
  const bool regular = style.layout == lockscreen_login_box::LayoutMode::Regular;

  if (m_panel != nullptr) {
    m_panel->setPosition(0.0F, 0.0F);
    m_panel->setSize(panelWidth, panelHeight);
    m_panel->setStyle(
        RoundedRectStyle{
            .fill = resolveColorSpec(style.panelFill),
            .border = colorForRole(ColorRole::Outline, style.panelOpacity),
            .fillMode = FillMode::Solid,
            .radius = Style::scaledRadius(style.panelRadius),
            .softness = 1.0F,
            .borderWidth = Style::borderWidth,
        }
    );
  }

  const float padV = Style::spaceLg;
  const float padH = Style::spaceLg;
  const bool showSession = regular && style.showSessionButtons;
  const bool showMedia = style.showMedia;
  const bool showWeather = style.showWeather;
  const bool showInfoExtras = showMedia || showWeather;
  const lockscreen_login_box::RegularRowHeights rows = regular
      ? lockscreen_login_box::regularRowHeights(panelHeight, showSession, showInfoExtras)
      : lockscreen_login_box::RegularRowHeights{};

  float contentTop = padV;
  if (regular) {
    if (m_infoGhost != nullptr) {
      m_infoGhost->setVisible(showInfoExtras);
      if (showInfoExtras) {
        m_infoGhost->setPosition(padH, contentTop);
        m_infoGhost->setSize(panelWidth - padH * 2.0F, rows.info);
        m_infoGhost->setStyle(
            RoundedRectStyle{
                .fill = colorForRole(ColorRole::Surface, 0.35F),
                .fillMode = FillMode::Solid,
                .radius = Style::scaledRadius(style.inputRadius),
            }
        );
      }
    }
    const float halfGap = Style::spaceSm;
    const float contentWidth = panelWidth - padH * 2.0F;
    const bool mediaAlone = showMedia && !showWeather;
    const bool weatherAlone = showWeather && !showMedia;
    const float mediaWidth = lockscreen_login_box::infoExtraBudget(contentWidth, showMedia, showWeather);
    const float weatherWidth = lockscreen_login_box::infoExtraBudget(contentWidth, showWeather, showMedia);
    const float ghostHeight = std::max(0.0F, rows.info - Style::spaceXs * 2.0F);
    if (m_mediaGhost != nullptr) {
      m_mediaGhost->setVisible(showMedia);
      if (showMedia) {
        const float ghostWidth =
            mediaAlone ? std::max(40.0F, contentWidth * 0.55F) : std::max(0.0F, mediaWidth - Style::spaceXs);
        const float ghostX = mediaAlone ? padH + (contentWidth - ghostWidth) * 0.5F : padH + Style::spaceXs;
        m_mediaGhost->setPosition(ghostX, contentTop + Style::spaceXs);
        m_mediaGhost->setSize(ghostWidth, ghostHeight);
        m_mediaGhost->setStyle(
            RoundedRectStyle{
                .fill = colorForRole(ColorRole::Primary, 0.18F),
                .fillMode = FillMode::Solid,
                .radius = Style::scaledRadius(style.inputRadius),
            }
        );
      }
    }
    if (m_weatherGhost != nullptr) {
      m_weatherGhost->setVisible(showWeather);
      if (showWeather) {
        const float ghostWidth =
            weatherAlone ? std::max(40.0F, contentWidth * 0.7F) : std::max(0.0F, weatherWidth - Style::spaceXs);
        const float ghostX = weatherAlone ? padH + (contentWidth - ghostWidth) * 0.5F
            : showMedia                   ? padH + mediaWidth + halfGap + Style::spaceXs
                                          : padH + Style::spaceXs;
        m_weatherGhost->setPosition(ghostX, contentTop + Style::spaceXs);
        m_weatherGhost->setSize(ghostWidth, ghostHeight);
        m_weatherGhost->setStyle(
            RoundedRectStyle{
                .fill = colorForRole(ColorRole::Secondary, 0.18F),
                .fillMode = FillMode::Solid,
                .radius = Style::scaledRadius(style.inputRadius),
            }
        );
      }
    }
    if (showInfoExtras) {
      contentTop += rows.info + Style::spaceSm;
    }
  } else {
    if (m_infoGhost != nullptr) {
      m_infoGhost->setVisible(false);
    }
    if (m_mediaGhost != nullptr) {
      m_mediaGhost->setVisible(false);
    }
    if (m_weatherGhost != nullptr) {
      m_weatherGhost->setVisible(false);
    }
  }

  if (m_statusGhost != nullptr) {
    const bool showUnlockHint = style.showUnlockHint;
    m_statusGhost->setVisible(showUnlockHint);
    if (showUnlockHint) {
      const float layoutScale = regular ? rows.scale : 1.0F;
      const float authFontSize = Style::fontSizeCaption * layoutScale + layoutScale;
      const float authPadV = Style::spaceMd * layoutScale;
      const float authH = authPadV * 2.0F + authFontSize;
      const float authGap = Style::spaceSm;
      const float centerY = m_screenHeight > 0.0F ? m_panelCenterY : screenHeight * 0.5F;
      const float panelTop = centerY - panelHeight * 0.5F;
      const float authYAbove = panelTop - authGap - authH;
      const bool placeAbove = authYAbove >= Style::spaceLg;
      const float authY = placeAbove ? -(authGap + authH) : panelHeight + authGap;
      m_statusGhost->setPosition(0.0F, authY);
      m_statusGhost->setSize(panelWidth, authH);
      m_statusGhost->setZIndex(2);
      m_statusGhost->setStyle(
          RoundedRectStyle{
              .fill = resolveColorSpec(style.panelFill),
              .border = colorForRole(ColorRole::Outline, style.panelOpacity),
              .fillMode = FillMode::Solid,
              .radius = Style::scaledRadius(style.panelRadius),
              .softness = 1.0F,
              .borderWidth = Style::borderWidth,
          }
      );
    }
  }

  float passwordTop = contentTop;
  float passwordHeight = Style::controlHeight;
  float contentLeft = Style::spaceLg;
  float inputWidth = 0.0F;
  float buttonX = 0.0F;
  if (regular) {
    passwordHeight = rows.password;
    const float contentWidth = panelWidth - Style::spaceLg * 2.0F;
    const float buttonWidth = style.showLoginButton ? passwordHeight : 0.0F;
    const float gap = Style::spaceSm;
    inputWidth =
        style.showLoginButton ? std::max(120.0F, contentWidth - buttonWidth - gap) : std::max(120.0F, contentWidth);
    buttonX = contentLeft + inputWidth + gap;
  } else {
    const float contentWidth = panelWidth - Style::spaceLg * 2.0F;
    const float buttonWidth = style.showLoginButton ? passwordHeight : 0.0F;
    const float gap = Style::spaceSm;
    inputWidth =
        style.showLoginButton ? std::max(120.0F, contentWidth - buttonWidth - gap) : std::max(120.0F, contentWidth);
    buttonX = contentLeft + inputWidth + gap;
    const float remaining = std::max(passwordHeight, panelHeight - contentTop - padV);
    passwordTop = contentTop + std::max(0.0F, (remaining - passwordHeight) * 0.5F);
  }

  if (m_passwordGhost != nullptr) {
    m_passwordGhost->setPosition(contentLeft, passwordTop);
    m_passwordGhost->setSize(inputWidth, passwordHeight);
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
      m_loginButtonGhost->setPosition(buttonX, passwordTop);
      m_loginButtonGhost->setSize(passwordHeight, passwordHeight);
      m_loginButtonGhost->setStyle(
          RoundedRectStyle{
              .fill = colorForRole(ColorRole::Primary, 0.9F),
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
          buttonX + (passwordHeight - kLoginGlyphSize) * 0.5F, passwordTop + (passwordHeight - kLoginGlyphSize) * 0.5F
      );
      m_loginGlyph->measure(renderer);
    }
  }

  const float sessionTop = passwordTop + passwordHeight + (showSession ? Style::spaceSm : 0.0F);
  const float sessionGap = Style::spaceSm;
  const float sessionWidth = (panelWidth - padH * 2.0F - sessionGap * static_cast<float>(m_sessionGhosts.size() - 1))
      / static_cast<float>(m_sessionGhosts.size());
  for (std::size_t i = 0; i < m_sessionGhosts.size(); ++i) {
    Box* ghost = m_sessionGhosts[i];
    if (ghost == nullptr) {
      continue;
    }
    ghost->setVisible(showSession);
    if (!showSession) {
      continue;
    }
    ghost->setPosition(padH + static_cast<float>(i) * (sessionWidth + sessionGap), sessionTop);
    ghost->setSize(sessionWidth, rows.session);
    ghost->setStyle(
        RoundedRectStyle{
            .fill = colorForRole(ColorRole::Surface, 0.55F),
            .fillMode = FillMode::Solid,
            .radius = Style::scaledRadius(style.inputRadius),
        }
    );
  }

  if (Node* rootNode = root()) {
    rootNode->setSize(panelWidth, panelHeight);
  }
}
