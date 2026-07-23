#include "shell/session/session_panel.h"

#include "config/config_service.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "render/scene/input_area.h"
#include "shell/panel/panel_manager.h"
#include "shell/session/session_action_meta.h"
#include "shell/session/session_action_runner.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/countdown_ring.h"
#include "ui/controls/flex.h"
#include "ui/controls/grid_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace {

  constexpr Logger kLog("session");
  constexpr float kCountdownScrimAlpha = 0.58f;

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
  // The min-width floor keeps the default single-row panel comfortably wide; an explicit
  // grid sizes to its own columns instead of being stretched back out to it.
  return scaled(gridEnabled() ? w : std::max(kPanelMinWidth, w));
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

bool SessionPanel::gridEnabled() const { return m_config != nullptr && m_config->config().shell.session.grid; }

std::size_t SessionPanel::visibleColumnCount() const {
  const std::size_t n = std::max<std::size_t>(1, entryCountForLayout());
  if (gridEnabled()) {
    const auto columns = static_cast<std::size_t>(std::max(1, m_config->config().shell.session.gridColumns));
    return std::min(columns, n);
  }
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

  m_visibleButtons.clear();
  m_countdownOverlays.clear();
  m_entryShortcutBadges.clear();
  m_visibleButtons.reserve(m_visibleEntries.size());
  m_countdownOverlays.reserve(m_visibleEntries.size());
  m_entryShortcutBadges.reserve(m_visibleEntries.size());
  for (std::size_t i = 0; i < m_visibleEntries.size(); ++i) {
    const auto& cfg = m_visibleEntries[i];
    if (cfg.shortcut.has_value() && cfg.shortcut->sym != 0) {
      m_entryShortcutBadges.emplace_back(keyChordDisplayLabel(*cfg.shortcut));
    } else {
      m_entryShortcutBadges.emplace_back();
    }
    if (Button* b = createActionButton(cfg, i, scale); b != nullptr) {
      ActionCountdownOverlay overlay{};
      attachCountdownOverlay(*b, overlay, scale);
      m_visibleButtons.push_back(b);
      m_countdownOverlays.push_back(overlay);
      rootLayout->addChild(std::unique_ptr<Button>(b));
    }
  }

  setRoot(std::move(rootLayout));

  if (m_animations != nullptr) {
    root()->setAnimationManager(m_animations);
  }

  updateSelectionVisuals();
}

Button* SessionPanel::createActionButton(const SessionPanelActionConfig& cfg, std::size_t index, float scale) {
  auto button = std::make_unique<Button>();
  const std::string labelText =
      cfg.label.has_value() && !cfg.label->empty() ? *cfg.label : i18n::tr(session_action::labelKey(cfg.action));
  button->setText(labelText);
  if (index < m_entryShortcutBadges.size()
      && m_entryShortcutBadges[index].has_value()
      && m_config->config().shell.session.showShortcuts) {
    button->setBadge(*m_entryShortcutBadges[index]);
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
  button->setTabStop(false);

  button->setOnClick([this, index]() { armEntry(index); });
  button->setOnEnter([this, index]() {
    if (m_pendingCountdown.has_value() && m_pendingCountdown->index != index) {
      cancelCountdown();
    }
  });
  button->setOnMotion([this, index]() {
    if (m_pendingCountdown.has_value() && m_pendingCountdown->index != index) {
      cancelCountdown();
    }
  });

  return button.release();
}

void SessionPanel::attachCountdownOverlay(Button& button, ActionCountdownOverlay& overlay, float scale) {
  const float ringSize = 64.0f * scale;

  auto overlayRoot = std::make_unique<Flex>();
  overlayRoot->setDirection(FlexDirection::Vertical);
  overlayRoot->setAlign(FlexAlign::Center);
  overlayRoot->setJustify(FlexJustify::Center);
  overlayRoot->setParticipatesInLayout(false);
  overlayRoot->setZIndex(0);
  overlayRoot->setVisible(false);
  overlay.root = overlayRoot.get();

  auto scrim = std::make_unique<Box>();
  scrim->setRadius(Style::scaledRadiusLg(scale));
  scrim->setParticipatesInLayout(false);
  scrim->setZIndex(0);
  overlay.scrim = scrim.get();
  overlayRoot->addChild(std::move(scrim));

  auto ring = std::make_unique<CountdownRing>();
  ring->setRingSize(ringSize);
  ring->setThickness(std::max(5.0f, 5.5f * scale));
  ring->setFontSize(22.0f * scale);
  ring->setParticipatesInLayout(false);
  ring->setZIndex(1);
  overlay.ring = ring.get();
  overlayRoot->addChild(std::move(ring));

  button.addChild(std::move(overlayRoot));
}

void SessionPanel::syncCountdownOverlayColors(std::size_t index) {
  if (index >= m_countdownOverlays.size() || index >= m_visibleEntries.size()) {
    return;
  }
  ActionCountdownOverlay& overlay = m_countdownOverlays[index];
  const SessionActionButtonVariant variant = m_visibleEntries[index].variant;
  const Button::ButtonPalette buttonPalette = Button::defaultPalette(buttonVariantFor(variant));
  const Button::ButtonStateColors& state = buttonPalette.pressed;

  ColorSpec scrimFill = state.bg;
  scrimFill.alpha *= kCountdownScrimAlpha;

  if (overlay.scrim != nullptr) {
    overlay.scrim->setFill(scrimFill);
  }
  if (overlay.ring != nullptr) {
    overlay.ring->setColor(state.label);
  }
}

void SessionPanel::onPanelCardOpacityChanged(float opacity) {
  for (Button* button : m_visibleButtons) {
    if (button != nullptr) {
      button->setSurfaceOpacity(opacity);
    }
  }
}

void SessionPanel::onOpen(std::string_view /*context*/) {
  m_selectedIndex.reset();
  m_pendingCountdown.reset();
  hideCountdownOverlays();
  restoreEntryBadges();
  updateSelectionVisuals();
}

void SessionPanel::armEntry(std::size_t index) {
  if (index >= m_visibleEntries.size()) {
    return;
  }

  const SessionPanelActionConfig& cfg = m_visibleEntries[index];
  if (cfg.countdownSeconds <= 0.0) {
    executeEntry(index);
    return;
  }

  if (m_pendingCountdown.has_value() && m_pendingCountdown->index == index) {
    executeEntry(index);
    return;
  }

  cancelCountdown();
  m_pendingCountdown = PendingCountdown{
      .index = index,
      .remainingMs = cfg.countdownSeconds * 1000.0,
      .totalMs = cfg.countdownSeconds * 1000.0,
  };
  m_selectedIndex = index;
  updateSelectionVisuals();
  updateCountdownVisuals();
  PanelManager::instance().requestLayout();
  PanelManager::instance().requestFrameTick();
  PanelManager::instance().refresh();
}

void SessionPanel::executeEntry(std::size_t index) {
  if (index >= m_visibleEntries.size()) {
    return;
  }
  const SessionPanelActionConfig cfg = m_visibleEntries[index];
  m_pendingCountdown.reset();
  PanelManager::instance().close();
  invokeEntry(cfg);
}

void SessionPanel::cancelCountdown() {
  if (!m_pendingCountdown.has_value()) {
    return;
  }
  m_pendingCountdown.reset();
  hideCountdownOverlays();
  restoreEntryBadges();
  updateSelectionVisuals();
  if (root() != nullptr) {
    root()->markPaintDirty();
  }
  PanelManager::instance().refresh();
}

void SessionPanel::hideCountdownOverlays() {
  for (auto& overlay : m_countdownOverlays) {
    if (overlay.root != nullptr) {
      overlay.root->setVisible(false);
    }
  }
}

void SessionPanel::restoreEntryBadges() {
  for (std::size_t i = 0; i < m_visibleButtons.size(); ++i) {
    Button* button = m_visibleButtons[i];
    if (button == nullptr) {
      continue;
    }
    if (i < m_entryShortcutBadges.size()
        && m_entryShortcutBadges[i].has_value()
        && m_config->config().shell.session.showShortcuts) {
      button->setBadge(*m_entryShortcutBadges[i]);
    } else {
      button->setBadge("");
    }
  }
}

void SessionPanel::updateCountdownVisuals() {
  hideCountdownOverlays();
  restoreEntryBadges();

  if (!m_pendingCountdown.has_value()) {
    return;
  }

  const std::size_t pendingIndex = m_pendingCountdown->index;
  if (pendingIndex >= m_countdownOverlays.size()) {
    return;
  }

  const int seconds = std::max(1, static_cast<int>(std::ceil(m_pendingCountdown->remainingMs / 1000.0)));
  const float progress = m_pendingCountdown->totalMs > 0.0
      ? static_cast<float>(std::clamp(m_pendingCountdown->remainingMs / m_pendingCountdown->totalMs, 0.0, 1.0))
      : 0.0f;

  ActionCountdownOverlay& overlay = m_countdownOverlays[pendingIndex];
  if (overlay.root != nullptr) {
    overlay.root->setVisible(true);
  }
  syncCountdownOverlayColors(pendingIndex);
  if (overlay.ring != nullptr) {
    overlay.ring->setProgress(progress);
    overlay.ring->setSeconds(seconds);
  }
  if (pendingIndex < m_visibleButtons.size()) {
    if (Button* button = m_visibleButtons[pendingIndex]; button != nullptr) {
      button->setBadge("");
    }
  }
}

void SessionPanel::layoutCountdownOverlays(Renderer& renderer) {
  for (std::size_t i = 0; i < m_visibleButtons.size() && i < m_countdownOverlays.size(); ++i) {
    Button* button = m_visibleButtons[i];
    ActionCountdownOverlay& overlay = m_countdownOverlays[i];
    if (button == nullptr || overlay.root == nullptr) {
      continue;
    }

    const float width = button->width();
    const float height = button->height();
    overlay.root->setPosition(0.0f, 0.0f);
    overlay.root->setFrameSize(width, height);

    if (overlay.scrim != nullptr) {
      overlay.scrim->setPosition(0.0f, 0.0f);
      overlay.scrim->setFrameSize(width, height);
      overlay.scrim->setSize(width, height);
    }

    if (overlay.ring != nullptr) {
      const float ringSize = overlay.ring->ringSize();
      const float ringX = (width - ringSize) * 0.5f;
      const float ringY = (height - ringSize) * 0.5f;
      overlay.ring->setPosition(ringX, ringY);
      overlay.ring->layout(renderer);
    }
  }
}

void SessionPanel::onFrameTick(float deltaMs) {
  if (!m_pendingCountdown.has_value()) {
    return;
  }

  m_pendingCountdown->remainingMs -= static_cast<double>(deltaMs);
  if (m_pendingCountdown->remainingMs <= 0.0) {
    const std::size_t index = m_pendingCountdown->index;
    executeEntry(index);
    return;
  }

  updateCountdownVisuals();
  PanelManager::instance().requestLayout();
  if (root() != nullptr) {
    root()->markPaintDirty();
  }
  PanelManager::instance().requestFrameTick();
  PanelManager::instance().refresh();
}

bool SessionPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  if (!pressed || preedit) {
    return false;
  }

  if (KeybindMatcher::matches(KeybindAction::Cancel, sym, modifiers)) {
    if (m_pendingCountdown.has_value()) {
      cancelCountdown();
      return true;
    }
    return false;
  }

  if (m_visibleButtons.empty()) {
    return false;
  }

  for (std::size_t i = 0; i < m_visibleEntries.size(); ++i) {
    const auto& entryConfig = m_visibleEntries[i];
    if (entryConfig.shortcut.has_value() && keyChordMatches(*entryConfig.shortcut, sym, modifiers)) {
      armEntry(i);
      return true;
    }
  }

  const std::size_t lastIndex = m_visibleButtons.size() - 1;

  const auto cancelCountdownOnSelectionChange = [this](std::optional<std::size_t> nextIndex) {
    if (m_pendingCountdown.has_value() && (!nextIndex.has_value() || *nextIndex != m_pendingCountdown->index)) {
      cancelCountdown();
    }
  };

  const auto moveSelection = [this, &cancelCountdownOnSelectionChange](std::size_t index) {
    cancelCountdownOnSelectionChange(index);
    m_selectedIndex = index;
    updateSelectionVisuals();
    if (root() != nullptr) {
      root()->markPaintDirty();
    }
    PanelManager::instance().refresh();
    return true;
  };

  if (KeybindMatcher::matches(KeybindAction::Left, sym, modifiers)) {
    if (!m_selectedIndex.has_value()) {
      return moveSelection(lastIndex);
    }
    if (*m_selectedIndex > 0) {
      return moveSelection(*m_selectedIndex - 1);
    }
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Right, sym, modifiers)) {
    if (!m_selectedIndex.has_value()) {
      return moveSelection(0);
    }
    if (*m_selectedIndex < lastIndex) {
      return moveSelection(*m_selectedIndex + 1);
    }
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Up, sym, modifiers)) {
    const std::size_t columns = visibleColumnCount();
    if (!m_selectedIndex.has_value()) {
      return moveSelection(lastIndex);
    }
    if (*m_selectedIndex >= columns) {
      return moveSelection(*m_selectedIndex - columns);
    }
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Down, sym, modifiers)) {
    const std::size_t columns = visibleColumnCount();
    if (!m_selectedIndex.has_value()) {
      return moveSelection(0);
    }
    if (*m_selectedIndex + columns <= lastIndex) {
      return moveSelection(*m_selectedIndex + columns);
    }
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
    if (!m_selectedIndex.has_value()) {
      return false;
    }
    Button* button = m_visibleButtons[*m_selectedIndex];
    if (button != nullptr && button->enabled()) {
      armEntry(*m_selectedIndex);
      return true;
    }
  }

  return false;
}

void SessionPanel::invokeEntry(const SessionPanelActionConfig& cfg) {
  if (m_actionRunner == nullptr) {
    kLog.warn("session panel: action runner unavailable");
    return;
  }
  m_actionRunner->invoke(cfg);
}

void SessionPanel::updateSelectionVisuals() {
  for (std::size_t i = 0; i < m_visibleButtons.size(); ++i) {
    Button* button = m_visibleButtons[i];
    if (button == nullptr) {
      continue;
    }
    const bool countdownActive = m_pendingCountdown.has_value() && m_pendingCountdown->index == i;
    if (countdownActive) {
      button->setSelected(false);
      button->setHoveredVisual(false);
      button->setPressedVisual(true);
      continue;
    }

    button->setHoveredVisual(false);
    button->setPressedVisual(false);
    const bool keyboardSelected = m_selectedIndex.has_value() && i == *m_selectedIndex;
    button->setSelected(keyboardSelected);
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
  layoutCountdownOverlays(renderer);
}

void SessionPanel::doUpdate(Renderer& /*renderer*/) {}

void SessionPanel::onClose() {
  m_pendingCountdown.reset();
  m_selectedIndex.reset();
  m_rootLayout = nullptr;
  m_visibleEntries.clear();
  m_visibleButtons.clear();
  m_countdownOverlays.clear();
  m_entryShortcutBadges.clear();
  clearReleasedRoot();
}
