#include "shell/polkit/polkit_panel.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "core/files/resource_paths.h"
#include "core/input/key_modifiers.h"
#include "core/input/keybind_matcher.h"
#include "dbus/polkit/polkit_agent.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>

namespace {

  int wrappedLineCount(std::string_view text, int charsPerLine, int maxLines) {
    if (text.empty()) {
      return 0;
    }
    int lines = 0;
    int col = 0;
    for (char ch : text) {
      if (ch == '\n') {
        ++lines;
        col = 0;
        if (lines >= maxLines) {
          return maxLines;
        }
        continue;
      }
      ++col;
      if (charsPerLine > 0 && col > charsPerLine) {
        ++lines;
        col = 1;
        if (lines >= maxLines) {
          return maxLines;
        }
      }
    }
    if (col > 0 || lines == 0) {
      ++lines;
    }
    return std::min(lines, maxLines);
  }

  std::string wrapLongRuns(std::string text, std::size_t maxRun = 48) {
    std::string out;
    out.reserve(text.size() + text.size() / maxRun);
    std::size_t run = 0;
    for (char ch : text) {
      const bool breakable = std::isspace(static_cast<unsigned char>(ch)) != 0 || ch == '/' || ch == ':' || ch == '-';
      out.push_back(ch);
      if (breakable) {
        run = 0;
        continue;
      }
      ++run;
      if (run >= maxRun) {
        out.push_back('\n');
        run = 0;
      }
    }
    return out;
  }

} // namespace

PolkitPanel::PolkitPanel(ConfigService* config, std::function<PolkitAgent*()> agentProvider)
    : m_config(config), m_agentProvider(std::move(agentProvider)) {}

PanelPlacement PolkitPanel::panelPlacement() const noexcept {
  return m_config != nullptr ? m_config->config().shell.panel.polkitPlacement : PanelPlacement::Floating;
}

float PolkitPanel::preferredHeight() const {
  const float scale = contentScale();
  const float bodyLine = Style::fontSizeBody * scale * 1.35f;
  const float titleLine = Style::fontSizeTitle * scale * 1.35f;
  const float captionLine = Style::fontSizeCaption * scale * 1.35f;
  const float iconSize = scaled(48.0f);
  const float pad = Style::spaceLg * scale;
  const float gapMd = Style::spaceMd * scale;
  const float gapSm = Style::spaceSm * scale;

  const float contentW = preferredWidth() - scaled(Style::panelPadding) * 2.0f;
  const float innerW = std::max(1.0f, contentW - pad * 2.0f);
  const float messageW = std::max(1.0f, innerW - iconSize - gapMd);
  const float avgChar = Style::fontSizeBody * scale * 0.55f;
  const int messageChars = std::max(1, static_cast<int>(messageW / avgChar));
  const int promptChars = std::max(1, static_cast<int>(innerW / avgChar));

  int messageLines = 1;
  int promptLines = 1;
  int supplementaryLines = 0;

  if (PolkitAgent* agent = m_agentProvider != nullptr ? m_agentProvider() : nullptr;
      agent != nullptr && agent->hasPendingRequest()) {
    const PolkitRequest request = agent->pendingRequest();
    const std::string message = wrapLongRuns(request.message.empty() ? request.actionId : request.message);
    messageLines = std::max(1, wrappedLineCount(message, messageChars, 6));

    const std::string supplementaryRaw = agent->supplementaryMessage();
    const bool supplementaryError = agent->supplementaryIsError();
    std::string promptText = wrapLongRuns(agent->inputPrompt());
    std::string supplementaryText = wrapLongRuns(supplementaryRaw);
    if (!agent->isResponseRequired() && !supplementaryText.empty() && !supplementaryError) {
      promptText = supplementaryText;
      supplementaryText.clear();
    } else if (
        !supplementaryText.empty()
        && (supplementaryError || supplementaryText == i18n::tr("auth.polkit.authenticating"))
    ) {
      promptText = supplementaryText;
      supplementaryText.clear();
    }
    // Preferred height is applied once at open. The password request arrives right after
    // BeginAuthentication, so always reserve prompt+input chrome even if responseRequired
    // is still false when the panel is first opened.
    promptLines = std::max(1, wrappedLineCount(promptText, promptChars, 3));
    supplementaryLines = wrappedLineCount(supplementaryText, promptChars, 4);
  }

  const float top = std::max(iconSize, titleLine + static_cast<float>(messageLines) * bodyLine);
  float bottom = static_cast<float>(promptLines) * bodyLine + gapSm;
  bottom += Style::controlHeight * scale + gapSm;
  if (supplementaryLines > 0) {
    bottom += static_cast<float>(supplementaryLines) * captionLine + gapSm;
  }
  bottom += Style::controlHeight * scale;

  return std::ceil(pad * 2.0f + top + gapMd + bottom + scaled(Style::panelPadding) * 2.0f + gapSm);
}

void PolkitPanel::create() {
  const float scale = contentScale();
  const float iconSize = scaled(48.0f);
  auto root = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .padding = Style::spaceLg * scale,
  });

  auto focusArea = std::make_unique<InputArea>();
  focusArea->setFocusable(true);
  focusArea->setVisible(false);
  m_focusArea = static_cast<InputArea*>(root->addChild(std::move(focusArea)));

  auto iconContainer = ui::node({
      .out = &m_iconContainer,
      .width = iconSize,
      .height = iconSize,
  });
  auto iconFallback = ui::glyph({
      .out = &m_fallbackIcon,
      .glyph = "shield-lock",
      .glyphSize = iconSize * 0.65f,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
  });
  iconContainer->addChild(std::move(iconFallback));
  auto iconImage = ui::image({
      .out = &m_icon,
      .fit = ImageFit::Contain,
      .visible = false,
  });
  iconContainer->addChild(std::move(iconImage));

  auto topContent = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceMd * scale}, std::move(iconContainer),
      ui::column(
          {.align = FlexAlign::Stretch, .flexGrow = 1.0f},
          ui::label({
              .out = &m_titleLabel,
              .text = i18n::tr("auth.polkit.title"),
              .fontSize = Style::fontSizeTitle * scale,
              .fontWeight = FontWeight::Bold,
              .color = colorSpecFromRole(ColorRole::Primary),
          }),
          ui::label({
              .out = &m_messageLabel,
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 6,
          })
      )
  );
  root->addChild(std::move(topContent));

  auto bottomContent = ui::column(
      {.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale},
      ui::label({
          .out = &m_promptLabel,
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 3,
      }),
      ui::input({
          .out = &m_input,
          .placeholder = i18n::tr("auth.polkit.password-placeholder"),
          .passwordMode = true,
          .surfaceOpacity = panelCardOpacity(),
          .onSubmit = [this](const std::string&) { submit(); },
          .onKeyEvent =
              [this](std::uint32_t sym, std::uint32_t modifiers) { return handleInputKeyEvent(sym, modifiers); },
      }),
      ui::label({
          .out = &m_supplementaryLabel,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 4,
      }),
      ui::row(
          {
              .align = FlexAlign::Center,
              .justify = FlexJustify::End,
              .wrap = true,
              .gap = Style::spaceSm * scale,
              .fillWidth = true,
          },
          ui::button({
              .out = &m_cancelButton,
              .text = i18n::tr("common.actions.cancel"),
              .variant = ButtonVariant::Outline,
              .onClick = []() { PanelManager::instance().close(); },
          }),
          ui::button({
              .out = &m_submitButton,
              .text = i18n::tr("auth.polkit.authenticate"),
              .variant = ButtonVariant::Primary,
              .onClick = [this]() { submit(); },
          })
      )
  );
  root->addChild(std::move(bottomContent));
  setRoot(std::move(root));
}

void PolkitPanel::onOpen(std::string_view /*context*/) {
  m_lastResponseRequired = false;
  m_iconResolved = false;
  if (m_input != nullptr) {
    m_input->setValue("");
  }
}

void PolkitPanel::onClose() {
  if (PolkitAgent* agent = m_agentProvider != nullptr ? m_agentProvider() : nullptr; agent != nullptr) {
    if (agent->hasPendingRequest()) {
      agent->cancelRequest();
    }
  }
  m_lastResponseRequired = false;
  clearReleasedRoot();

  m_rootLayout = nullptr;
  m_focusArea = nullptr;
  m_titleLabel = nullptr;
  m_messageLabel = nullptr;
  m_promptLabel = nullptr;
  m_supplementaryLabel = nullptr;
  m_input = nullptr;
  m_submitButton = nullptr;
  m_cancelButton = nullptr;
  m_iconContainer = nullptr;
  m_icon = nullptr;
  m_fallbackIcon = nullptr;
}

InputArea* PolkitPanel::initialFocusArea() const {
  PolkitAgent* agent = m_agentProvider != nullptr ? m_agentProvider() : nullptr;
  if (agent != nullptr && !agent->isResponseRequired()) {
    return m_focusArea;
  }
  return m_input != nullptr ? m_input->inputArea() : m_focusArea;
}

void PolkitPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(width, height);
  m_rootLayout->layout(renderer);
  if (m_iconContainer != nullptr) {
    if (m_icon != nullptr && m_icon->visible()) {
      m_icon->setSize(m_iconContainer->width(), m_iconContainer->height());
      m_icon->setPosition(0.0f, 0.0f);
    }
    if (m_fallbackIcon != nullptr && m_fallbackIcon->visible()) {
      const float ox = std::round((m_iconContainer->width() - m_fallbackIcon->width()) * 0.5f);
      const float oy = std::round((m_iconContainer->height() - m_fallbackIcon->height()) * 0.5f);
      m_fallbackIcon->setPosition(ox, oy);
    }
  }
}

void PolkitPanel::doUpdate(Renderer& renderer) {
  PolkitAgent* agent = m_agentProvider != nullptr ? m_agentProvider() : nullptr;
  if (agent == nullptr
      || m_messageLabel == nullptr
      || m_promptLabel == nullptr
      || m_supplementaryLabel == nullptr
      || m_submitButton == nullptr
      || m_input == nullptr
      || m_icon == nullptr
      || m_fallbackIcon == nullptr) {
    return;
  }
  const PolkitRequest request = agent->pendingRequest();
  const bool needsInput = agent->isResponseRequired();
  const std::string supplementaryRaw = agent->supplementaryMessage();
  const bool supplementaryError = agent->supplementaryIsError();
  const bool isInvalidPassword = supplementaryError && supplementaryRaw == i18n::tr("auth.polkit.invalid-password");
  std::string promptText = wrapLongRuns(agent->inputPrompt());
  std::string supplementaryText = wrapLongRuns(supplementaryRaw);
  if (!needsInput && !supplementaryText.empty() && !supplementaryError) {
    promptText = supplementaryText;
    supplementaryText.clear();
  } else if (
      !supplementaryText.empty() && (supplementaryError || supplementaryText == i18n::tr("auth.polkit.authenticating"))
  ) {
    promptText = supplementaryText;
    supplementaryText.clear();
  }
  m_messageLabel->setText(wrapLongRuns(request.message.empty() ? request.actionId : request.message));
  m_promptLabel->setText(promptText);
  m_promptLabel->setColor(
      isInvalidPassword ? colorSpecFromRole(ColorRole::Error) : colorSpecFromRole(ColorRole::OnSurface)
  );
  m_promptLabel->setVisible(!promptText.empty());
  m_supplementaryLabel->setText(supplementaryText);
  m_supplementaryLabel->setVisible(!supplementaryText.empty());
  m_supplementaryLabel->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  m_input->setVisible(needsInput);
  m_submitButton->setEnabled(needsInput);
  if (needsInput && !m_lastResponseRequired) {
    if (auto* manager = PanelManager::current(); manager != nullptr && manager->isOpenPanel("polkit")) {
      manager->focusArea(m_input->inputArea());
    }
  }
  m_lastResponseRequired = needsInput;

  if (request.iconName != m_lastIconName || !m_iconResolved) {
    m_lastIconName = request.iconName;
    m_iconResolved = true;
    resolveIcon(renderer, request);
  }
}

void PolkitPanel::resolveIcon(Renderer& renderer, const PolkitRequest& request) {
  const float iconSize = scaled(48.0f);
  if (m_iconContainer != nullptr) {
    m_iconContainer->setSize(iconSize, iconSize);
  }

  if (request.isInternal) {
    const auto logoPath = paths::assetPath("noctalia.svg");
    m_fallbackIcon->setVisible(false);
    m_icon->setSize(iconSize, iconSize);
    m_icon->setSourceFile(renderer, logoPath.string(), static_cast<int>(std::round(iconSize)), true);
    m_icon->setVisible(true);
    return;
  }

  if (!request.iconName.empty()) {
    const std::string& resolved = m_iconResolver.resolve(request.iconName, static_cast<int>(std::round(iconSize)));
    if (!resolved.empty()) {
      m_fallbackIcon->setVisible(false);
      m_icon->setSize(iconSize, iconSize);
      m_icon->setSourceFile(renderer, resolved, static_cast<int>(std::round(iconSize)), true);
      m_icon->setVisible(true);
      return;
    }
  }

  m_icon->clear(renderer);
  m_icon->setVisible(false);
  m_fallbackIcon->setVisible(true);
}

void PolkitPanel::submit() {
  PolkitAgent* agent = m_agentProvider != nullptr ? m_agentProvider() : nullptr;
  if (agent == nullptr || m_input == nullptr) {
    return;
  }
  agent->submitResponse(m_input->value());
  m_input->setValue("");
}

bool PolkitPanel::handleInputKeyEvent(std::uint32_t sym, std::uint32_t modifiers) {
  if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
    submit();
    return true;
  }
  const bool shift = (modifiers & KeyMod::Shift) != 0;
  if (KeybindMatcher::matches(KeybindAction::Left, sym, modifiers)) {
    if (m_input != nullptr) {
      m_input->moveCaretLeft(shift);
    }
    return true;
  }
  if (KeybindMatcher::matches(KeybindAction::Right, sym, modifiers)) {
    if (m_input != nullptr) {
      m_input->moveCaretRight(shift);
    }
    return true;
  }
  return false;
}

void PolkitPanel::onPanelCardOpacityChanged(float opacity) {
  if (m_input != nullptr) {
    m_input->setSurfaceOpacity(opacity);
  }
}
