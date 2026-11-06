#include "shell/desktop/widgets/desktop_welcome_widget.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/profile/avatar_path.h"
#include "system/distro_info.h"
#include "ui/builders.h"
#include "ui/controls/image.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/clamp.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

  constexpr int kMinAvatarSize = 48;
  constexpr int kMaxAvatarSize = 192;
  constexpr float kSpacing = 20.0F;
  constexpr float kTextGap = 4.0F;
  constexpr float kMaxTextWidth = 320.0F;
  constexpr float kShadowAlpha = 0.6F;
  constexpr float kShadowOffset = 1.5F;

  float greetingFontSize(float contentScale) { return Style::fontSizeBody * contentScale; }
  float nameFontSize(float contentScale) { return Style::fontSizeBody * 1.5F * contentScale; }

} // namespace

DesktopWelcomeWidget::DesktopWelcomeWidget(AccountsService* accounts, ConfigService* config, Options options)
    : m_accounts(accounts), m_config(config), m_showAvatar(options.showAvatar), m_showName(options.showName),
      m_nameText(std::move(options.nameText)), m_greetingText(std::move(options.greetingText)),
      m_avatarSize(util::clampOrdered(options.avatarSize, kMinAvatarSize, kMaxAvatarSize)),
      m_avatarShape(options.avatarShape), m_showAvatarBorder(options.showAvatarBorder), m_color(options.color),
      m_shadow(options.shadow) {}

void DesktopWelcomeWidget::create() {
  auto rootNode = ui::node({});
  rootNode->setClipChildren(true);

  auto avatar = ui::image({
      .out = &m_avatar,
      .fit = ImageFit::Cover,
  });
  rootNode->addChild(std::move(avatar));

  auto greetingLabel = ui::label({
      .out = &m_greetingLabel,
      .text = m_greetingText.empty() ? i18n::tr("desktop-widgets.welcome.greeting") : m_greetingText,
      .fontSize = greetingFontSize(contentScale()),
      .color = m_color,
      .maxLines = 1,
      .textAlign = TextAlign::Start,
  });
  rootNode->addChild(std::move(greetingLabel));

  auto nameLabel = ui::label(
      {.out = &m_nameLabel,
       .text = std::string{},
       .fontSize = nameFontSize(contentScale()),
       .fontWeight = FontWeight::Bold,
       .color = m_color,
       .maxLines = 1,
       .textAlign = TextAlign::Start}
  );
  rootNode->addChild(std::move(nameLabel));

  m_avatar->setVisible(false);

  setRoot(std::move(rootNode));
  syncName();
  applyTextColors();
  applyShadow();
}

bool DesktopWelcomeWidget::applySetting(
    const std::string& key, const WidgetSettingValue& value,
    const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
) {
  if (key == "show_avatar") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_showAvatar = *v;
      requestUpdate();
      return true;
    }
    return false;
  }
  if (key == "show_name") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_showName = *v;
      syncName();
      return true;
    }
    return false;
  }
  if (key == "name_text") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_nameText = *v;
      syncName();
      return true;
    }
    return false;
  }
  if (key == "greeting_text") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_greetingText = *v;
      if (m_greetingLabel != nullptr) {
        const std::string text = m_greetingText.empty() ? i18n::tr("desktop-widgets.welcome.greeting") : m_greetingText;
        m_greetingLabel->setText(text);
      }
      requestLayout();
      return true;
    }
    return false;
  }
  if (key == "avatar_size") {
    if (const auto* v = std::get_if<std::int64_t>(&value)) {
      m_avatarSize = static_cast<int>(*v);
      requestLayout();
      return true;
    }
    return false;
  }
  if (key == "avatar_shape") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_avatarShape = *v == "square" ? AvatarShape::Square : AvatarShape::Circular;
      requestLayout();
      return true;
    }
    return false;
  }
  if (key == "show_avatar_border") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_showAvatarBorder = *v;
      applyAvatarChrome();
      return true;
    }
    return false;
  }
  if (key == "color") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_color = colorSpecFromConfigString(*v, key);
      applyTextColors();
      requestLayout();
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

void DesktopWelcomeWidget::onFontFamilyChanged(const std::string& family, Renderer& /*renderer*/) {
  if (m_greetingLabel != nullptr) {
    m_greetingLabel->setFontFamily(family);
  }
  if (m_nameLabel != nullptr) {
    m_nameLabel->setFontFamily(family);
  }
}

void DesktopWelcomeWidget::doUpdate(Renderer& renderer) {
  refreshAvatar(renderer);
  syncName();
}

void DesktopWelcomeWidget::refreshAvatar(Renderer& renderer) {
  if (m_avatar == nullptr) {
    return;
  }

  std::string displayPath;
  if (m_showAvatar && m_config != nullptr) {
    displayPath = shell::avatarDisplayPath(m_accounts, m_config->config());
  }
  const bool hasAvatar = !displayPath.empty();
  if (hasAvatar != m_avatarVisible) {
    m_avatarVisible = hasAvatar;
    m_avatar->setVisible(hasAvatar);
    requestLayout();
  }

  if (displayPath.empty()) {
    if (!m_lastAvatarPath.empty()) {
      m_avatar->clear(renderer);
      m_lastAvatarPath.clear();
    }
    return;
  }

  m_lastAvatarPath = displayPath;
  // The control early-returns on unchanged sources, so this is cheap per update.
  const int avatarPx = static_cast<int>(std::round(static_cast<float>(m_avatarSize) * contentScale()));
  if (!m_avatar->setSourceFile(renderer, displayPath, avatarPx, false, true)) {
    m_avatar->clear(renderer);
  }
}

void DesktopWelcomeWidget::syncName() {
  if (m_nameLabel == nullptr) {
    return;
  }
  const std::string name = m_showName ? (m_nameText.empty() ? sessionUserName() : m_nameText) : std::string{};
  if (name == m_lastName) {
    return;
  }
  const bool visibilityChanged = name.empty() != m_lastName.empty();
  m_lastName = name;
  m_nameLabel->setText(name);
  m_nameLabel->setVisible(!name.empty());
  if (visibilityChanged) {
    requestLayout();
  }
}

void DesktopWelcomeWidget::doLayout(Renderer& renderer) {
  if (root() == nullptr || m_avatar == nullptr || m_greetingLabel == nullptr || m_nameLabel == nullptr) {
    return;
  }

  const float scale = contentScale();
  const float avatarSize = static_cast<float>(m_avatarSize) * scale;
  const float spacing = kSpacing * scale;
  const float textGap = kTextGap * scale;
  const float maxWidth = std::max(1.0F, kMaxTextWidth * scale);
  const bool showAvatar = m_avatarVisible;

  float textX = 0.0F;
  if (showAvatar) {
    m_avatar->setSize(avatarSize, avatarSize);
    m_avatar->setRadius(m_avatarShape == AvatarShape::Circular ? avatarSize * 0.5F : Style::scaledRadiusLg(scale));
    applyAvatarChrome();
    m_avatar->setPosition(0.0F, 0.0F);
    textX = avatarSize + spacing;
  }

  m_greetingLabel->setFontSize(greetingFontSize(scale));
  m_greetingLabel->setMaxWidth(maxWidth);
  m_greetingLabel->measure(renderer);
  const float greetingHeight = m_greetingLabel->height();
  float textWidth = m_greetingLabel->width();
  float textBlockHeight = greetingHeight;

  float nameGap = 0.0F;
  float nameHeight = 0.0F;
  if (m_nameLabel->visible()) {
    m_nameLabel->setFontSize(nameFontSize(scale));
    m_nameLabel->setMaxWidth(maxWidth);
    m_nameLabel->measure(renderer);
    nameGap = textGap;
    nameHeight = m_nameLabel->height();
    textBlockHeight += nameGap + nameHeight;
    textWidth = std::max(textWidth, m_nameLabel->width());
  }

  const float blockHeight = std::max(showAvatar ? avatarSize : 0.0F, textBlockHeight);
  const float textY = std::round(std::max(0.0F, (blockHeight - textBlockHeight) * 0.5F));

  m_greetingLabel->setPosition(textX, textY);
  if (m_nameLabel->visible()) {
    m_nameLabel->setPosition(textX, textY + greetingHeight + nameGap);
  }

  root()->setSize(textX + textWidth, blockHeight);
  applyTextColors();
  applyShadow();
}

void DesktopWelcomeWidget::applyTextColors() {
  if (m_greetingLabel != nullptr) {
    m_greetingLabel->setColor(m_color);
  }
  if (m_nameLabel != nullptr) {
    m_nameLabel->setColor(m_color);
  }
}

void DesktopWelcomeWidget::applyShadow() {
  const float offset = kShadowOffset * contentScale();
  const ColorSpec shadowColor = colorSpecFromRole(ColorRole::Shadow, kShadowAlpha);

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

  applyTo(m_greetingLabel);
  applyTo(m_nameLabel);
}

void DesktopWelcomeWidget::applyAvatarChrome() {
  if (m_avatar == nullptr) {
    return;
  }
  if (m_showAvatarBorder) {
    m_avatar->setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth * 3.0F);
    return;
  }
  m_avatar->setBorder(colorSpecFromRole(ColorRole::Primary), 0.0F);
}
