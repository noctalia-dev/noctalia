#include "shell/session/session_panel.h"

#include "config/config_service.h"
#include "core/keybind_matcher.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/panel/panel_manager.h"
#include "shell/session/session_action_meta.h"
#include "shell/session/session_action_runner.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/grid_view.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace {

  constexpr Logger kLog("session");

  [[nodiscard]] ButtonVariant buttonVariantFor(SessionActionButtonVariant variant) {
    switch (variant) {
    case SessionActionButtonVariant::Default:
      return ButtonVariant::Default;
    case SessionActionButtonVariant::Primary:
      return ButtonVariant::Primary;
    case SessionActionButtonVariant::Secondary:
      return ButtonVariant::Secondary;
    case SessionActionButtonVariant::Destructive:
      return ButtonVariant::Destructive;
    case SessionActionButtonVariant::Outline:
      return ButtonVariant::Outline;
    case SessionActionButtonVariant::Ghost:
      return ButtonVariant::Ghost;
    }
    return ButtonVariant::Default;
  }

} // namespace

std::vector<SessionPanelActionConfig> SessionPanel::effectiveActions() const {
  std::vector<SessionPanelActionConfig> src =
      m_config != nullptr ? m_config->config().shell.session.actions : defaultSessionPanelActions();

  std::vector<SessionPanelActionConfig> out;
  out.reserve(src.size());
  for (const auto& row : src) {
    if (!row.enabled) {
      continue;
    }
    if (!session_action::isKnown(row.action)) {
      kLog.warn("session panel: skipping unknown action \"{}\"", row.action);
      continue;
    }
    if (row.action == "command" && (!row.command.has_value() || StringUtils::trim(*row.command).empty())) {
      kLog.warn("session panel: skipping \"command\" entry with no command");
      continue;
    }
    if ((row.action == "lock" || row.action == "lock_and_suspend")
        && m_config != nullptr
        && !m_config->isLockScreenEnabled()) {
      continue;
    }
    out.push_back(row);
  }
  return out;
}

PanelPlacement SessionPanel::panelPlacement() const noexcept {
  return m_config != nullptr ? m_config->config().shell.panel.sessionPlacement : PanelPlacement::Attached;
}

float SessionPanel::preferredWidth() const {
  const std::size_t n = visibleColumnCount();
  const float gap = Style::spaceSm;
  const float w = kButtonMinWidth * static_cast<float>(n)
      + gap * static_cast<float>(n > 1 ? n - 1 : 0)
      + Style::panelPadding * 2.0f;
  return scaled(std::max(kPanelMinWidth, w));
}

float SessionPanel::preferredHeight() const {
  const std::size_t rows = visibleRowCount();
  const float gap = Style::spaceSm;
  const float h = kActionButtonMinHeight * static_cast<float>(rows)
      + gap * static_cast<float>(rows > 1 ? rows - 1 : 0)
      + Style::panelPadding * 2.0f;
  return std::ceil(scaled(h));
}

std::size_t SessionPanel::entryCountForLayout() const {
  if (!m_visibleEntries.empty()) {
    return m_visibleEntries.size();
  }
  return effectiveActions().size();
}

std::size_t SessionPanel::visibleColumnCount() const {
  const std::size_t n = std::max<std::size_t>(1, entryCountForLayout());
  if (n <= kMaxColumns) {
    return n;
  }
  return std::min<std::size_t>(kMaxColumns, (n + 1) / 2);
}

std::size_t SessionPanel::visibleRowCount() const {
  const std::size_t n = std::max<std::size_t>(1, entryCountForLayout());
  const std::size_t columns = visibleColumnCount();
  return (n + columns - 1) / columns;
}

void SessionPanel::create() {
  const float scale = contentScale();
  m_visibleEntries = effectiveActions();
  const std::size_t columns = visibleColumnCount();

  auto rootLayout = std::make_unique<GridView>();
  rootLayout->setColumns(columns);
  rootLayout->setColumnGap(Style::spaceSm * scale);
  rootLayout->setRowGap(Style::spaceSm * scale);
  rootLayout->setStretchItems(true);
  rootLayout->setUniformCellSize(true);
  rootLayout->setMinCellWidth(kButtonMinWidth * scale);
  rootLayout->setMinCellHeight(kActionButtonMinHeight * scale);
  m_rootLayout = rootLayout.get();

  auto focusArea = std::make_unique<InputArea>();
  focusArea->setFocusable(true);
  focusArea->setVisible(false);
  focusArea->setOnKeyDown([this](const InputArea::KeyData& key) {
    if (key.pressed) {
      handleKeyEvent(key.sym, key.modifiers);
    }
  });
  m_focusArea = static_cast<InputArea*>(rootLayout->addChild(std::move(focusArea)));

  m_visibleButtons.clear();
  m_visibleButtons.reserve(m_visibleEntries.size());
  for (const auto& cfg : m_visibleEntries) {
    if (Button* b = createActionButton(cfg, scale); b != nullptr) {
      m_visibleButtons.push_back(b);
      rootLayout->addChild(std::unique_ptr<Button>(b));
    }
  }

  setRoot(std::move(rootLayout));

  if (m_animations != nullptr) {
    root()->setAnimationManager(m_animations);
  }

  updateSelectionVisuals();
}

Button* SessionPanel::createActionButton(const SessionPanelActionConfig& cfg, float scale) {
  auto button = std::make_unique<Button>();
  const std::string labelText =
      cfg.label.has_value() && !cfg.label->empty() ? *cfg.label : i18n::tr(session_action::labelKey(cfg.action));
  button->setText(labelText);
  if (cfg.shortcut.has_value() && cfg.shortcut->sym != 0) {
    button->setBadge(keyChordDisplayLabel(*cfg.shortcut));
  }
  button->setGlyph(
      cfg.glyph.has_value() && !cfg.glyph->empty() ? *cfg.glyph : session_action::defaultGlyph(cfg.action)
  );
  button->setVariant(buttonVariantFor(cfg.variant));
  button->setSurfaceOpacity(panelCardOpacity());
  button->setDirection(FlexDirection::Vertical);
  button->setAlign(FlexAlign::Center);
  button->setJustify(FlexJustify::Center);
  button->setGap(Style::spaceSm * scale);
  button->setContentAlign(ButtonContentAlign::Center);
  button->setFontSize((Style::fontSizeBody + 1.0f) * scale);
  button->setGlyphSize(28.0f * scale);
  button->setPadding(Style::spaceMd * scale, Style::spaceLg * scale);
  button->setRadius(Style::scaledRadiusLg(scale));
  button->setMinWidth(kButtonMinWidth * scale);
  button->setMinHeight(kActionButtonMinHeight * scale);
  button->setFlexGrow(1.0f);

  const SessionPanelActionConfig& cfgCopy = cfg;
  button->setOnClick([this, cfgCopy]() {
    PanelManager::instance().close();
    invokeEntry(cfgCopy);
  });
  button->setOnMotion([this]() { activateMouse(); });
  button->setHoverSuppressed(!m_mouseActive);

  return button.release();
}

void SessionPanel::onPanelCardOpacityChanged(float opacity) {
  for (Button* button : m_visibleButtons) {
    if (button != nullptr) {
      button->setSurfaceOpacity(opacity);
    }
  }
}

InputArea* SessionPanel::initialFocusArea() const { return m_focusArea; }

void SessionPanel::onOpen(std::string_view /*context*/) {
  m_selectedIndex.reset();
  m_mouseActive = false;
  updateSelectionVisuals();
}

void SessionPanel::activateMouse() {
  if (m_mouseActive) {
    return;
  }
  m_mouseActive = true;
  for (Button* button : m_visibleButtons) {
    if (button != nullptr) {
      button->setHoverSuppressed(false);
    }
  }
  PanelManager::instance().refresh();
}

void SessionPanel::activateSelected() {
  if (!m_selectedIndex.has_value() || m_visibleButtons.empty()) {
    return;
  }
  const std::size_t i = *m_selectedIndex;
  if (i >= m_visibleButtons.size() || i >= m_visibleEntries.size()) {
    return;
  }
  Button* button = m_visibleButtons[i];
  if (button != nullptr && button->enabled()) {
    PanelManager::instance().close();
    invokeEntry(m_visibleEntries[i]);
  }
}

void SessionPanel::invokeEntry(const SessionPanelActionConfig& cfg) {
  if (m_actionRunner == nullptr) {
    kLog.warn("session panel: action runner unavailable");
    return;
  }
  m_actionRunner->invoke(cfg);
}

bool SessionPanel::handleKeyEvent(std::uint32_t sym, std::uint32_t modifiers) {
  if (m_visibleButtons.empty()) {
    return false;
  }

  for (const auto& entryConfig : m_visibleEntries) {
    if (entryConfig.shortcut.has_value() && keyChordMatches(*entryConfig.shortcut, sym, modifiers)) {
      PanelManager::instance().close();
      invokeEntry(entryConfig);
      return true;
    }
  }

  const std::size_t lastIndex = m_visibleButtons.size() - 1;

  if (KeybindMatcher::matches(KeybindAction::Left, sym, modifiers)) {
    if (!m_selectedIndex.has_value()) {
      m_selectedIndex = lastIndex;
      updateSelectionVisuals();
      if (root() != nullptr) {
        root()->markPaintDirty();
      }
      PanelManager::instance().refresh();
    } else if (*m_selectedIndex > 0) {
      --(*m_selectedIndex);
      updateSelectionVisuals();
      if (root() != nullptr) {
        root()->markPaintDirty();
      }
      PanelManager::instance().refresh();
    }
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Right, sym, modifiers)) {
    if (!m_selectedIndex.has_value()) {
      m_selectedIndex = 0;
      updateSelectionVisuals();
      if (root() != nullptr) {
        root()->markPaintDirty();
      }
      PanelManager::instance().refresh();
    } else if (*m_selectedIndex < lastIndex) {
      ++(*m_selectedIndex);
      updateSelectionVisuals();
      if (root() != nullptr) {
        root()->markPaintDirty();
      }
      PanelManager::instance().refresh();
    }
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Up, sym, modifiers)) {
    const std::size_t columns = visibleColumnCount();
    if (!m_selectedIndex.has_value()) {
      m_selectedIndex = lastIndex;
    } else if (*m_selectedIndex >= columns) {
      *m_selectedIndex -= columns;
    }
    updateSelectionVisuals();
    if (root() != nullptr) {
      root()->markPaintDirty();
    }
    PanelManager::instance().refresh();
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Down, sym, modifiers)) {
    const std::size_t columns = visibleColumnCount();
    if (!m_selectedIndex.has_value()) {
      m_selectedIndex = 0;
    } else if (*m_selectedIndex + columns <= lastIndex) {
      *m_selectedIndex += columns;
    }
    updateSelectionVisuals();
    if (root() != nullptr) {
      root()->markPaintDirty();
    }
    PanelManager::instance().refresh();
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
    activateSelected();
    return true;
  }

  return false;
}

void SessionPanel::updateSelectionVisuals() {
  for (std::size_t i = 0; i < m_visibleButtons.size(); ++i) {
    Button* button = m_visibleButtons[i];
    if (button == nullptr) {
      continue;
    }
    button->setSelected(m_selectedIndex.has_value() && i == *m_selectedIndex);
  }
}

void SessionPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_rootLayout == nullptr) {
    return;
  }

  m_rootLayout->setSize(width, height);
  m_rootLayout->layout(renderer);

  for (Button* button : m_visibleButtons) {
    if (button != nullptr) {
      button->updateInputArea();
    }
  }
}

void SessionPanel::doUpdate(Renderer& /*renderer*/) {}

void SessionPanel::onClose() {
  m_rootLayout = nullptr;
  m_focusArea = nullptr;
  m_visibleEntries.clear();
  m_visibleButtons.clear();
  clearReleasedRoot();
}
