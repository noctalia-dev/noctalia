#include "shell/auth/auth_panel.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "core/files/resource_paths.h"
#include "core/input/key_modifiers.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/auth/auth_source.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

namespace {

  constexpr Logger kLog("auth_panel");

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

AuthPanel::AuthPanel(ConfigService* config, ActiveSourceProvider activeProvider)
    : m_config(config), m_activeProvider(std::move(activeProvider)) {}

PanelPlacement AuthPanel::panelPlacement() const noexcept {
  return m_config != nullptr ? m_config->config().shell.panel.authPlacement : PanelPlacement::Floating;
}

const AuthSource* AuthPanel::activeOrNull() const { return m_activeProvider != nullptr ? m_activeProvider() : nullptr; }
AuthSource* AuthPanel::activeOrNull() { return m_activeProvider != nullptr ? m_activeProvider() : nullptr; }

const std::string AuthPanel::freshSessionId() const {
  auto source = activeOrNull();
  if (source == nullptr)
    return "";
  return source->sessionId();
}

bool AuthPanel::refreshAuthView() const {
  auto source = activeOrNull();
  if (source == nullptr) {
    kLog.warn("failed to refresh AuthView: AuthSource is null");
    return false;
  } else {
    kLog.debug("creating AuthView");
    m_cachedView = source->view();
    m_isCachedViewValid = true;
    return true;
  }
}

const AuthView& AuthPanel::currentView() const {
  if (!m_isCachedViewValid && !refreshAuthView()) {
    static const AuthView emptyView{};
    return emptyView;
  }
  return m_cachedView;
}

std::string AuthPanel::authCancelLabel() const { return i18n::tr("common.actions.cancel"); }

std::string AuthPanel::authIconKey(AuthIconKind iconKind, const std::string& iconValue) const {
  switch (iconKind) {
  case AuthIconKind::InternalLogo:
    return "internal-logo";
  case AuthIconKind::ThemeIcon:
    return "theme:" + iconValue;
  case AuthIconKind::FilePath:
    return "file:" + iconValue;
  case AuthIconKind::GlyphIcon:
    return "glyph:" + iconValue;
  default:
    return {};
  }
}

void AuthPanel::onAuthSubmit(std::string_view response) {
  auto source = activeOrNull();
  if (source == nullptr || response.empty()) {
    return;
  }
  source->submit(std::string(response));
}

void AuthPanel::onAuthCancel() {
  auto source = activeOrNull();
  if (source != nullptr) {
    source->cancel();
  }
}

void AuthPanel::showGlyphIcon(Renderer& renderer, const std::string_view glyph) {
  const float iconSize = authIconSize();
  if (m_iconContainer != nullptr) {
    m_iconContainer->setSize(iconSize, iconSize);
  }
  if (m_icon != nullptr) {
    m_icon->clear(renderer);
    m_icon->setVisible(false);
  }
  if (m_glyphIcon != nullptr) {
    m_glyphIcon->setVisible(true);
    m_glyphIcon->setGlyph(glyph);
  }
}

void AuthPanel::showImageIcon(Renderer& renderer, const std::string& path, float iconSize) {
  if (m_iconContainer != nullptr) {
    m_iconContainer->setSize(iconSize, iconSize);
  }
  if (m_glyphIcon != nullptr) {
    m_glyphIcon->setVisible(false);
  }
  if (m_icon != nullptr) {
    m_icon->setSize(iconSize, iconSize);
    m_icon->setSourceFile(renderer, path, static_cast<int>(std::round(iconSize)), true);
    m_icon->setVisible(true);
  }
}

void AuthPanel::refreshAuthIcon(Renderer& renderer) {
  const float iconSize = authIconSize();
  const AuthView view = currentView();
  if (view.iconKind == AuthIconKind::InternalLogo) {
    const auto logoPath = paths::assetPath("noctalia.svg");
    showImageIcon(renderer, logoPath.string(), iconSize);
    return;
  }
  if (view.iconKind == AuthIconKind::FilePath && !view.iconValue.empty()) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(view.iconValue, ec) && !ec) {
      showImageIcon(renderer, view.iconValue, iconSize);
      return;
    }
    const std::string& resolved = m_iconResolver.resolve(view.iconValue, static_cast<int>(std::round(iconSize)));
    if (!resolved.empty()) {
      showImageIcon(renderer, resolved, iconSize);
      return;
    }
  } else if (view.iconKind == AuthIconKind::ThemeIcon && !view.iconValue.empty()) {
    const std::string& resolved = m_iconResolver.resolve(view.iconValue, static_cast<int>(std::round(iconSize)));
    if (!resolved.empty()) {
      showImageIcon(renderer, resolved, iconSize);
      return;
    }
  } else if (view.iconKind == AuthIconKind::GlyphIcon && !view.iconValue.empty()) {
    showGlyphIcon(renderer, view.iconValue);
    return;
  }
}

float AuthPanel::preferredWidth() const { return scaled(480.0F); }

float AuthPanel::preferredHeight() const {
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
  const int promptChars = std::max(1, static_cast<int>(innerW / avgChar));

  const auto view = currentView();
  const std::string message = wrapLongRuns(view.message);
  const int messageLines = message.empty() ? 0 : std::max(1, wrappedLineCount(message, messageChars, 6));
  const std::string promptText = wrapLongRuns(view.prompt);
  const int promptLines = std::max(1, wrappedLineCount(promptText, promptChars, 3));
  const std::string supplementaryText = wrapLongRuns(view.supplementary);
  const int supplementaryLines = wrappedLineCount(supplementaryText, promptChars, 4);

  const float top = std::max(iconSize, titleLine + static_cast<float>(messageLines) * bodyLine);
  float bottom = static_cast<float>(promptLines) * bodyLine + gapSm;
  if (view.needsInput) {
    bottom += Style::controlHeight * scale + gapSm;
  }
  if (supplementaryLines > 0) {
    bottom += static_cast<float>(supplementaryLines) * captionLine + gapSm;
  }
  bottom += Style::controlHeight * scale;

  return std::ceil(pad * 2.0F + top + gapMd + bottom + scaled(Style::panelPadding) * 2.0F + gapSm);
}

void AuthPanel::create() {
  const float scale = contentScale();
  const float iconSize = scaled(48.0F);
  auto root = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .padding = Style::spaceLg * scale,
  });
  const auto view = currentView();

  auto focusArea = ui::inputArea({});
  focusArea->setFocusable(true);
  focusArea->setVisible(false);
  m_focusArea = static_cast<InputArea*>(root->addChild(std::move(focusArea)));

  auto iconContainer = ui::node({
      .out = &m_iconContainer,
      .width = iconSize,
      .height = iconSize,
  });
  auto iconGlyph = ui::glyph({
      .out = &m_glyphIcon,
      // .glyph = view.iconValue,
      .glyphSize = iconSize * 0.65F,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
  });
  iconContainer->addChild(std::move(iconGlyph));
  auto iconImage = ui::image({
      .out = &m_icon,
      .fit = ImageFit::Contain,
      .visible = false,
  });
  iconContainer->addChild(std::move(iconImage));

  auto topContent = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceMd * scale}, std::move(iconContainer),
      ui::column(
          {.align = FlexAlign::Stretch, .flexGrow = 1.0F},
          ui::label({
              .out = &m_titleLabel,
              .text = view.title,
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
          .placeholder = view.placeholder,
          .passwordMode = view.passwordMode,
          .surfaceOpacity = panelCardOpacity(),
          .onChange =
              [this, needsInput = view.needsInput](const std::string& value) {
                if (m_submitButton != nullptr) {
                  m_submitButton->setEnabled(needsInput && !value.empty());
                }
              },
          .onSubmit = [this](const std::string& value) { submit(value); },
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
              .text = authCancelLabel(),
              .variant = ButtonVariant::Outline,
              .onClick =
                  [this]() {
                    onAuthCancel();
                    PanelManager::instance().close();
                  },
          }),
          ui::button({
              .out = &m_submitButton,
              .text = view.submitLabel,
              .variant = ButtonVariant::Primary,
              .onClick = [this]() { submit(); },
          })
      )
  );
  root->addChild(std::move(bottomContent));
  setRoot(std::move(root));
}

void AuthPanel::onOpen(std::string_view /*context*/) {
  const auto view = currentView();
  m_cachedView = view;
  m_isCachedViewValid = true;
  m_lastResponseRequired = false;
  m_lastPasswordMode = view.passwordMode;
  m_iconResolved = false;
  m_hasTrackedSession = false;
  m_trackedSessionId.clear();
  const std::string sessionId = view.sessionId;
  if (!sessionId.empty()) {
    m_trackedSessionId = sessionId;
    m_hasTrackedSession = true;
  }
  if (m_input != nullptr) {
    m_input->setValue("");
    m_input->setPasswordMode(m_lastPasswordMode);
  }
}

void AuthPanel::onClose() {
  const std::string sessionId = currentView().sessionId;
  if (m_hasTrackedSession && !sessionId.empty() && sessionId == m_trackedSessionId) {
    onAuthCancel();
  }
  m_hasTrackedSession = false;
  m_trackedSessionId.clear();
  m_lastResponseRequired = false;
  m_lastPasswordMode = true;
  m_isCachedViewValid = false;
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
  m_glyphIcon = nullptr;
}

bool AuthPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool /*preedit*/) {
  if (!pressed || !KeybindMatcher::matches(KeybindAction::Cancel, sym, modifiers)) {
    return false;
  }
  onAuthCancel();
  if (!freshSessionId().empty()) {
    return true;
  }
  PanelManager::instance().close();
  return true;
}

InputArea* AuthPanel::initialFocusArea() const {
  if (!currentView().needsInput) {
    return m_focusArea;
  }
  return m_input != nullptr ? m_input->inputArea() : m_focusArea;
}

void AuthPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(width, height);
  m_rootLayout->layout(renderer);
  if (m_iconContainer != nullptr) {
    if (m_icon != nullptr && m_icon->visible()) {
      m_icon->setSize(m_iconContainer->width(), m_iconContainer->height());
      m_icon->setPosition(0.0F, 0.0F);
    }
    if (m_glyphIcon != nullptr && m_glyphIcon->visible()) {
      const float ox = std::round((m_iconContainer->width() - m_glyphIcon->width()) * 0.5F);
      const float oy = std::round((m_iconContainer->height() - m_glyphIcon->height()) * 0.5F);
      m_glyphIcon->setPosition(ox, oy);
    }
  }
}

void AuthPanel::doUpdate(Renderer& renderer) {
  if (m_messageLabel == nullptr
      || m_promptLabel == nullptr
      || m_supplementaryLabel == nullptr
      || m_submitButton == nullptr
      || m_input == nullptr
      || m_icon == nullptr
      || m_glyphIcon == nullptr) {
    return;
  }

  const auto sessionId = freshSessionId();
  if (sessionId != m_trackedSessionId) {
    kLog.debug("new sessionId: {}", sessionId);
    if (!sessionId.empty() && m_hasTrackedSession) {
      m_input->setValue("");
    }
    m_trackedSessionId = sessionId;
    m_hasTrackedSession = !sessionId.empty();
    m_lastIconKey = {};
    m_iconResolved = false;

    m_isCachedViewValid = false;
    if (!refreshAuthView())
      return;
  }
  const auto view = currentView();
  const std::string message = wrapLongRuns(view.message);
  const std::string promptText = wrapLongRuns(view.prompt);
  const std::string supplementaryText = wrapLongRuns(view.supplementary);
  m_titleLabel->setText(view.title);
  m_titleLabel->setVisible(!view.title.empty());
  m_messageLabel->setText(message);
  m_messageLabel->setVisible(!message.empty());
  m_promptLabel->setText(promptText);
  m_promptLabel->setColor(
      view.promptIsError ? colorSpecFromRole(ColorRole::Error) : colorSpecFromRole(ColorRole::OnSurface)
  );
  m_promptLabel->setVisible(!promptText.empty());
  m_supplementaryLabel->setText(supplementaryText);
  m_supplementaryLabel->setVisible(!supplementaryText.empty());
  m_supplementaryLabel->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  m_input->setPlaceholder(view.placeholder);
  m_input->setVisible(view.needsInput);
  m_submitButton->setText(view.submitLabel);
  m_submitButton->setVisible(view.needsInput);
  m_submitButton->setEnabled(view.needsInput && !m_input->value().empty());
  if (view.passwordMode != m_lastPasswordMode) {
    m_input->setPasswordMode(view.passwordMode);
    m_lastPasswordMode = view.passwordMode;
  }
  if (view.needsInput != m_lastResponseRequired) {
    if (auto* manager = PanelManager::current(); manager != nullptr && manager->activePanel() == this) {
      manager->relayoutActivePanelPreferredSize();
      if (view.needsInput) {
        manager->focusArea(m_input->inputArea());
      }
    }
  }
  m_lastResponseRequired = view.needsInput;

  const std::string iconKey = authIconKey(view.iconKind, view.iconValue);
  if (iconKey != m_lastIconKey || !m_iconResolved) {
    m_lastIconKey = iconKey;
    m_iconResolved = true;
    refreshAuthIcon(renderer);
  }
}

void AuthPanel::submit(std::string_view response) {
  if (m_input == nullptr) {
    return;
  }
  const std::string secret = response.empty() ? m_input->value() : std::string(response);
  if (secret.empty()) {
    return;
  }
  onAuthSubmit(secret);
  m_input->setValue("");
}

bool AuthPanel::handleInputKeyEvent(std::uint32_t sym, std::uint32_t modifiers) {
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

void AuthPanel::onPanelCardOpacityChanged(float opacity) {
  if (m_input != nullptr) {
    m_input->setSurfaceOpacity(opacity);
  }
}
