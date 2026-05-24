#include "shell/polkit/polkit_panel.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "core/key_modifiers.h"
#include "core/keybind_matcher.h"
#include "dbus/polkit/polkit_agent.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <cctype>
#include <memory>

namespace {

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

PolkitPanel::PolkitPanel(ConfigService* /*config*/, std::function<PolkitAgent*()> agentProvider)
    : m_agentProvider(std::move(agentProvider)) {}

void PolkitPanel::create() {
  const float scale = contentScale();
  auto root = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .justify = FlexJustify::SpaceBetween,
      .padding = Style::spaceLg * scale,
  });

  auto focusArea = std::make_unique<InputArea>();
  focusArea->setFocusable(true);
  focusArea->setVisible(false);
  m_focusArea = static_cast<InputArea*>(root->addChild(std::move(focusArea)));

  auto topContent = ui::column(
      {.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale},
      ui::label({
          .out = &m_titleLabel,
          .text = i18n::tr("auth.polkit.title"),
          .fontSize = Style::fontSizeTitle * scale,
          .color = colorSpecFromRole(ColorRole::Primary),
          .fontWeight = FontWeight::Bold,
      }),
      ui::label({
          .out = &m_messageLabel,
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 6,
      })
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
          {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm * scale},
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
  clearReleasedRoot();
}

InputArea* PolkitPanel::initialFocusArea() const { return m_input != nullptr ? m_input->inputArea() : m_focusArea; }

void PolkitPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(width, height);
  // Labels auto-wrap via Flex Stretch propagation (root → topContent/bottomContent → labels).
  m_rootLayout->layout(renderer);
}

void PolkitPanel::doUpdate(Renderer& /*renderer*/) {
  PolkitAgent* agent = m_agentProvider != nullptr ? m_agentProvider() : nullptr;
  if (agent == nullptr || m_messageLabel == nullptr || m_promptLabel == nullptr || m_supplementaryLabel == nullptr ||
      m_submitButton == nullptr || m_input == nullptr) {
    return;
  }
  const PolkitRequest request = agent->pendingRequest();
  const std::string supplementaryRaw = agent->supplementaryMessage();
  const bool supplementaryError = agent->supplementaryIsError();
  const bool isInvalidPassword = supplementaryError && supplementaryRaw == i18n::tr("auth.polkit.invalid-password");
  std::string promptText = wrapLongRuns(agent->inputPrompt());
  std::string supplementaryText = wrapLongRuns(supplementaryRaw);
  if (!supplementaryText.empty() &&
      (supplementaryError || supplementaryText == i18n::tr("auth.polkit.authenticating"))) {
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
  m_submitButton->setEnabled(agent->isResponseRequired());
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
