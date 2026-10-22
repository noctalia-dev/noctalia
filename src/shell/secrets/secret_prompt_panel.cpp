#include "shell/secrets/secret_prompt_panel.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "core/input/key_modifiers.h"
#include "core/input/keybind_matcher.h"
#include "dbus/secrets/secret_prompter.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/glyph.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "ui/text_wrap.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace {

  using ui::wrapLongRuns;
  using ui::wrappedLineCount;

  std::string orFallback(const std::string& value, const std::string& fallback) {
    return value.empty() ? fallback : value;
  }

} // namespace

SecretPromptPanel::SecretPromptPanel(ConfigService* config, std::function<SecretPrompter*()> prompterProvider)
    : m_config(config), m_prompterProvider(std::move(prompterProvider)) {}

PanelPlacement SecretPromptPanel::panelPlacement() const noexcept { return PanelPlacement::Floating; }

SecretPrompter* SecretPromptPanel::prompter() const {
  return m_prompterProvider != nullptr ? m_prompterProvider() : nullptr;
}

float SecretPromptPanel::preferredHeight() const {
  const float scale = contentScale();
  const float bodyLine = Style::fontSizeBody * scale * 1.35F;
  const float titleLine = Style::fontSizeTitle * scale * 1.35F;
  const float captionLine = Style::fontSizeCaption * scale * 1.35F;
  const float iconSize = scaled(48.0F);
  const float pad = Style::spaceLg * scale;
  const float gapMd = Style::spaceMd * scale;
  const float gapSm = Style::spaceSm * scale;

  const float contentW = preferredWidth() - scaled(Style::panelPadding) * 2.0F;
  const float innerW = std::max(1.0F, contentW - pad * 2.0F);
  const float messageW = std::max(1.0F, innerW - iconSize - gapMd);
  const float avgChar = Style::fontSizeBody * scale * 0.55F;
  const int messageChars = std::max(1, static_cast<int>(messageW / avgChar));
  const int bodyChars = std::max(1, static_cast<int>(innerW / avgChar));

  int messageLines = 1;
  int descriptionLines = 0;
  int warningLines = 0;
  int choiceLines = 0;
  bool needsPassword = false;
  bool passwordNew = false;

  SecretPrompter* service = prompter();
  if (service != nullptr && service->hasPendingPrompt()) {
    const SecretPromptRequest request = service->pendingPrompt();
    needsPassword = request.type == SecretPromptRequest::Type::Password;
    passwordNew = needsPassword && request.passwordNew;
    messageLines = std::max(
        1,
        wrappedLineCount(
            wrapLongRuns(orFallback(request.message, i18n::tr("auth.secret-prompt.default-message"))), messageChars, 6
        )
    );
    descriptionLines = wrappedLineCount(wrapLongRuns(request.description), bodyChars, 4);
    warningLines = wrappedLineCount(wrapLongRuns(request.warning), bodyChars, 3);
    choiceLines = wrappedLineCount(wrapLongRuns(request.choiceLabel), bodyChars, 2);
  }
  if (m_mismatch) {
    warningLines = std::max(warningLines, 1);
  }

  const float top = std::max(iconSize, titleLine + static_cast<float>(messageLines) * bodyLine);
  float bottom = 0.0F;
  if (descriptionLines > 0) {
    bottom += static_cast<float>(descriptionLines) * captionLine + gapSm;
  }
  if (needsPassword) {
    bottom += Style::controlHeight * scale + gapSm;
  }
  if (passwordNew) {
    bottom += Style::controlHeight * scale + gapSm;
  }
  if (warningLines > 0) {
    bottom += static_cast<float>(warningLines) * captionLine + gapSm;
  }
  if (choiceLines > 0) {
    bottom += std::max(static_cast<float>(choiceLines) * bodyLine, Style::controlHeight * scale) + gapSm;
  }
  bottom += Style::controlHeight * scale;

  return std::ceil(pad * 2.0F + top + gapMd + bottom + scaled(Style::panelPadding) * 2.0F + gapSm);
}

void SecretPromptPanel::create() {
  const float scale = contentScale();
  const float iconSize = scaled(48.0F);

  auto root = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .padding = Style::spaceLg * scale,
  });

  auto focusArea = ui::inputArea({});
  focusArea->setFocusable(true);
  focusArea->setVisible(false);
  m_focusArea = static_cast<InputArea*>(root->addChild(std::move(focusArea)));

  auto topContent = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceMd * scale},
      ui::glyph({
          .out = &m_icon,
          .glyph = "key-round",
          .glyphSize = iconSize * 0.65F,
          .color = colorSpecFromRole(ColorRole::Primary),
      }),
      ui::column(
          {.align = FlexAlign::Stretch, .flexGrow = 1.0F},
          ui::label({
              .out = &m_titleLabel,
              .text = i18n::tr("auth.secret-prompt.title"),
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
          .out = &m_descriptionLabel,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 4,
      }),
      ui::input({
          .out = &m_input,
          .placeholder = i18n::tr("auth.secret-prompt.password-placeholder"),
          .passwordMode = true,
          .surfaceOpacity = panelCardOpacity(),
          .onChange = [this](const std::string& /*value*/) { m_mismatch = false; },
          .onSubmit = [this](const std::string& /*value*/) { submit(); },
          .onKeyEvent =
              [this](std::uint32_t sym, std::uint32_t modifiers) { return handleInputKeyEvent(sym, modifiers); },
      }),
      ui::input({
          .out = &m_confirmInput,
          .placeholder = i18n::tr("auth.secret-prompt.confirm-placeholder"),
          .passwordMode = true,
          .surfaceOpacity = panelCardOpacity(),
          .visible = false,
          .onChange = [this](const std::string& /*value*/) { m_mismatch = false; },
          .onSubmit = [this](const std::string& /*value*/) { submit(); },
          .onKeyEvent =
              [this](std::uint32_t sym, std::uint32_t modifiers) { return handleInputKeyEvent(sym, modifiers); },
      }),
      ui::label({
          .out = &m_warningLabel,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::Error),
          .maxLines = 3,
          .visible = false,
      }),
      ui::row(
          {
              .out = &m_choiceRow,
              .align = FlexAlign::Center,
              .gap = Style::spaceSm * scale,
              .visible = false,
          },
          ui::checkbox({
              .out = &m_choiceCheckbox,
              .checked = false,
          }),
          ui::label({
              .out = &m_choiceLabel,
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 2,
              .flexGrow = 1.0F,
          })
      ),
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
              .onClick =
                  [this]() {
                    cancelPrompt();
                    PanelManager::instance().close();
                  },
          }),
          ui::button({
              .out = &m_continueButton,
              .text = i18n::tr("auth.secret-prompt.unlock"),
              .variant = ButtonVariant::Primary,
              .onClick = [this]() { submit(); },
          })
      )
  );
  root->addChild(std::move(bottomContent));
  setRoot(std::move(root));
}

void SecretPromptPanel::onOpen(std::string_view /*context*/) {
  m_mismatch = false;
  m_lastNeedsPassword = false;
  m_lastPasswordNew = false;
  if (m_input != nullptr) {
    m_input->setValue("");
  }
  if (m_confirmInput != nullptr) {
    m_confirmInput->setValue("");
  }
}

void SecretPromptPanel::onClose() {
  // Unlike polkit there is no chained-request burst to protect against: a client
  // that wants another prompt sends PerformPrompt again and the panel reopens.
  // Anything still on screen at close time is a dismissal, so answer "no" rather
  // than leave the caller blocked forever.
  cancelPrompt();

  m_mismatch = false;
  clearReleasedRoot();

  m_rootLayout = nullptr;
  m_focusArea = nullptr;
  m_icon = nullptr;
  m_titleLabel = nullptr;
  m_messageLabel = nullptr;
  m_descriptionLabel = nullptr;
  m_warningLabel = nullptr;
  m_input = nullptr;
  m_confirmInput = nullptr;
  m_choiceRow = nullptr;
  m_choiceCheckbox = nullptr;
  m_choiceLabel = nullptr;
  m_continueButton = nullptr;
  m_cancelButton = nullptr;
}

void SecretPromptPanel::cancelPrompt() {
  if (SecretPrompter* service = prompter(); service != nullptr && service->hasPendingPrompt()) {
    service->cancel();
  }
}

bool SecretPromptPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool /*preedit*/) {
  if (!pressed || !KeybindMatcher::matches(KeybindAction::Cancel, sym, modifiers)) {
    return false;
  }
  cancelPrompt();
  PanelManager::instance().close();
  return true;
}

InputArea* SecretPromptPanel::initialFocusArea() const {
  SecretPrompter* service = prompter();
  const bool needsPassword = service != nullptr
      && service->hasPendingPrompt()
      && service->pendingPrompt().type == SecretPromptRequest::Type::Password;
  if (!needsPassword) {
    return m_focusArea;
  }
  return m_input != nullptr ? m_input->inputArea() : m_focusArea;
}

void SecretPromptPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(width, height);
  m_rootLayout->layout(renderer);
}

void SecretPromptPanel::doUpdate(Renderer& /*renderer*/) {
  SecretPrompter* service = prompter();
  if (service == nullptr
      || m_messageLabel == nullptr
      || m_descriptionLabel == nullptr
      || m_warningLabel == nullptr
      || m_input == nullptr
      || m_confirmInput == nullptr
      || m_choiceRow == nullptr
      || m_choiceLabel == nullptr
      || m_choiceCheckbox == nullptr
      || m_continueButton == nullptr) {
    return;
  }
  if (!service->hasPendingPrompt()) {
    return;
  }

  const SecretPromptRequest request = service->pendingPrompt();
  const bool needsPassword = request.type == SecretPromptRequest::Type::Password;
  const bool passwordNew = needsPassword && request.passwordNew;

  if (m_titleLabel != nullptr) {
    m_titleLabel->setText(orFallback(request.title, i18n::tr("auth.secret-prompt.title")));
  }
  m_messageLabel->setText(wrapLongRuns(orFallback(request.message, i18n::tr("auth.secret-prompt.default-message"))));

  const std::string description = wrapLongRuns(request.description);
  m_descriptionLabel->setText(description);
  m_descriptionLabel->setVisible(!description.empty());

  const std::string warning = m_mismatch ? i18n::tr("auth.secret-prompt.mismatch") : wrapLongRuns(request.warning);
  m_warningLabel->setText(warning);
  m_warningLabel->setVisible(!warning.empty());

  m_input->setVisible(needsPassword);
  m_confirmInput->setVisible(passwordNew);

  const bool hasChoice = !request.choiceLabel.empty();
  m_choiceRow->setVisible(hasChoice);
  if (hasChoice) {
    m_choiceLabel->setText(wrapLongRuns(request.choiceLabel));
  }

  m_continueButton->setText(orFallback(
      request.continueLabel, i18n::tr(needsPassword ? "auth.secret-prompt.unlock" : "auth.secret-prompt.continue")
  ));
  if (m_cancelButton != nullptr) {
    m_cancelButton->setText(orFallback(request.cancelLabel, i18n::tr("common.actions.cancel")));
  }

  if (needsPassword != m_lastNeedsPassword || passwordNew != m_lastPasswordNew) {
    if (auto* manager = PanelManager::current(); manager != nullptr && manager->isOpenPanel("secret-prompt")) {
      manager->relayoutActivePanelPreferredSize();
      if (needsPassword) {
        manager->focusArea(m_input->inputArea());
      }
    }
  }
  m_lastNeedsPassword = needsPassword;
  m_lastPasswordNew = passwordNew;
}

void SecretPromptPanel::submit() {
  SecretPrompter* service = prompter();
  if (service == nullptr || !service->hasPendingPrompt()) {
    return;
  }

  const SecretPromptRequest request = service->pendingPrompt();
  const bool choiceChosen = m_choiceCheckbox != nullptr && !request.choiceLabel.empty() && m_choiceCheckbox->checked();

  if (request.type == SecretPromptRequest::Type::Confirm) {
    service->confirm(choiceChosen);
    return;
  }

  if (m_input == nullptr) {
    return;
  }
  const std::string password = m_input->value();
  if (request.passwordNew && m_confirmInput != nullptr && password != m_confirmInput->value()) {
    // Refuse rather than send a password the user did not mean to set: the caller
    // would store it and the user would be locked out of their own collection.
    m_mismatch = true;
    return;
  }

  m_mismatch = false;
  service->submitPassword(password, choiceChosen);
  m_input->setValue("");
  if (m_confirmInput != nullptr) {
    m_confirmInput->setValue("");
  }
}

bool SecretPromptPanel::handleInputKeyEvent(std::uint32_t sym, std::uint32_t modifiers) {
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

void SecretPromptPanel::onPanelCardOpacityChanged(float opacity) {
  if (m_input != nullptr) {
    m_input->setSurfaceOpacity(opacity);
  }
  if (m_confirmInput != nullptr) {
    m_confirmInput->setSurfaceOpacity(opacity);
  }
}
