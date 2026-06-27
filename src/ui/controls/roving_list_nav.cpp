#include "ui/controls/roving_list_nav.h"

#include "core/key_symbols.h"
#include "core/keybind_matcher.h"
#include "render/scene/input_area.h"
#include "ui/controls/button.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace {

  bool itemVisibleInScene(const Button* button) {
    if (button == nullptr) {
      return false;
    }
    for (const Node* current = button; current != nullptr; current = current->parent()) {
      if (!current->visible()) {
        return false;
      }
    }
    return true;
  }

} // namespace

void RovingListNavController::setOptions(Options options) { m_options = std::move(options); }

void RovingListNavController::bindFocusArea(InputArea* area) {
  m_focusArea = area;
  if (m_focusArea == nullptr) {
    return;
  }
  m_focusArea->setOnFocusGain([this]() { onFocusGain(); });
  m_focusArea->setOnFocusLoss([this]() { onFocusLoss(); });
  m_focusArea->setOnKeyDown([this](const InputArea::KeyData& key) {
    if (key.pressed) {
      handleKey(key.sym, key.modifiers);
    }
  });
}

void RovingListNavController::registerItem(Button* button, std::function<void()> activate) {
  if (button == nullptr) {
    return;
  }
  button->setTabStop(m_options.keepItemsInTabOrder);
  m_items.push_back(Item{button, std::move(activate)});
}

void RovingListNavController::clearItems() { m_items.clear(); }

void RovingListNavController::notifyExternalSelectionChanged() {
  if (m_focusArea != nullptr && m_focusArea->focused()) {
    applyKeyboardHints();
  }
}

void RovingListNavController::onFocusGain() {
  syncKeyboardIndexToSelection();
  applyKeyboardHints();
  scrollKeyboardItemIntoView();
}

void RovingListNavController::onFocusLoss() { clearKeyboardHints(); }

void RovingListNavController::handleKey(std::uint32_t sym, std::uint32_t modifiers) {
  if (m_items.empty()) {
    return;
  }

  const bool vertical = m_options.axis == RovingListNavAxis::Vertical;
  if (vertical && KeybindMatcher::matches(KeybindAction::Up, sym, modifiers)) {
    moveBy(-1);
    return;
  }
  if (vertical && KeybindMatcher::matches(KeybindAction::Down, sym, modifiers)) {
    moveBy(1);
    return;
  }
  if (!vertical && KeybindMatcher::matches(KeybindAction::Left, sym, modifiers)) {
    moveBy(-1);
    return;
  }
  if (!vertical && KeybindMatcher::matches(KeybindAction::Right, sym, modifiers)) {
    moveBy(1);
    return;
  }
  if (KeySymbol::isHome(sym)) {
    if (const auto index = firstNavigableIndex()) {
      setKeyboardIndex(*index, true);
      if (m_options.mode == RovingListNavMode::FollowFocus) {
        activateKeyboardIndex();
      }
    }
    return;
  }
  if (KeySymbol::isEnd(sym)) {
    for (std::size_t i = m_items.size(); i-- > 0;) {
      if (isNavigable(m_items[i])) {
        setKeyboardIndex(i, true);
        if (m_options.mode == RovingListNavMode::FollowFocus) {
          activateKeyboardIndex();
        }
        break;
      }
    }
    return;
  }
  if (m_options.mode == RovingListNavMode::Roving && KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
    activateKeyboardIndex();
  }
}

void RovingListNavController::layoutOverlay(float width, float height) {
  if (m_focusArea != nullptr) {
    m_focusArea->setPosition(0.0f, 0.0f);
    m_focusArea->setFrameSize(width, height);
  }
}

bool RovingListNavController::isNavigable(const Item& item) const {
  return item.button != nullptr && item.button->enabled() && itemVisibleInScene(item.button);
}

std::optional<std::size_t> RovingListNavController::firstNavigableIndex() const {
  for (std::size_t i = 0; i < m_items.size(); ++i) {
    if (isNavigable(m_items[i])) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> RovingListNavController::findNavigableIndex(std::size_t from, int direction) const {
  if (m_items.empty() || direction == 0) {
    return std::nullopt;
  }

  std::size_t index = std::min(from, m_items.size() - 1);
  for (std::size_t step = 0; step < m_items.size(); ++step) {
    if (direction > 0) {
      if (index + 1 >= m_items.size()) {
        if (!m_options.wrap) {
          return std::nullopt;
        }
        index = 0;
      } else {
        ++index;
      }
    } else {
      if (index == 0) {
        if (!m_options.wrap) {
          return std::nullopt;
        }
        index = m_items.size() - 1;
      } else {
        --index;
      }
    }
    if (isNavigable(m_items[index])) {
      return index;
    }
  }
  return std::nullopt;
}

void RovingListNavController::syncKeyboardIndexToSelection() {
  if (m_options.syncIndexFromSelection) {
    setKeyboardIndex(m_options.syncIndexFromSelection(), false);
    return;
  }

  for (std::size_t i = 0; i < m_items.size(); ++i) {
    if (isNavigable(m_items[i]) && m_items[i].button->variant() == ButtonVariant::TabActive) {
      setKeyboardIndex(i, false);
      return;
    }
  }

  if (const auto index = firstNavigableIndex()) {
    setKeyboardIndex(*index, false);
  }
}

void RovingListNavController::setKeyboardIndex(std::size_t index, bool scrollIntoView) {
  if (m_items.empty()) {
    return;
  }
  index = std::min(index, m_items.size() - 1);
  if (m_keyboardIndex != index) {
    if (m_keyboardIndex < m_items.size() && m_items[m_keyboardIndex].button != nullptr) {
      m_items[m_keyboardIndex].button->setKeyboardFocusHint(false);
    }
    m_keyboardIndex = index;
  }
  applyKeyboardHints();
  if (scrollIntoView) {
    scrollKeyboardItemIntoView();
  }
}

void RovingListNavController::applyKeyboardHints() {
  const bool focused = m_focusArea != nullptr && m_focusArea->focused();
  for (std::size_t i = 0; i < m_items.size(); ++i) {
    if (m_items[i].button != nullptr) {
      m_items[i].button->setKeyboardFocusHint(focused && i == m_keyboardIndex);
    }
  }
}

void RovingListNavController::clearKeyboardHints() {
  for (const Item& item : m_items) {
    if (item.button != nullptr) {
      item.button->setKeyboardFocusHint(false);
    }
  }
}

void RovingListNavController::scrollKeyboardItemIntoView() {
  if (m_keyboardIndex >= m_items.size() || m_items[m_keyboardIndex].button == nullptr) {
    return;
  }
  if (m_options.scrollIntoView) {
    m_options.scrollIntoView(m_items[m_keyboardIndex].button);
  }
}

void RovingListNavController::moveBy(int direction) {
  if (direction == 0) {
    return;
  }
  const auto next = findNavigableIndex(m_keyboardIndex, direction);
  if (!next.has_value()) {
    return;
  }
  setKeyboardIndex(*next, true);
  if (m_options.mode == RovingListNavMode::FollowFocus) {
    activateKeyboardIndex();
  }
}

void RovingListNavController::activateKeyboardIndex() {
  if (m_keyboardIndex < m_items.size() && m_items[m_keyboardIndex].activate) {
    m_items[m_keyboardIndex].activate();
  }
}

RovingListNavHost::RovingListNavHost(RovingListNavController::Options options) {
  const bool keepItemsInTabOrder = options.keepItemsInTabOrder;
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);

  auto area = std::make_unique<InputArea>();
  area->setFocusable(true);
  area->setTabStop(!keepItemsInTabOrder);
  area->setHitTestVisible(false);
  m_focusArea = static_cast<InputArea*>(addChild(std::move(area)));
  m_focusArea->setParticipatesInLayout(false);
  m_focusArea->setZIndex(2);

  m_controller.setOptions(std::move(options));
  m_controller.bindFocusArea(m_focusArea);
}

void RovingListNavHost::setTabFocusKey(std::string key) {
  if (m_focusArea != nullptr) {
    m_focusArea->setTabFocusKey(std::move(key));
  }
}

void RovingListNavHost::registerItem(Button* button, std::function<void()> activate) {
  m_controller.registerItem(button, std::move(activate));
}

void RovingListNavHost::clearItems() { m_controller.clearItems(); }

void RovingListNavHost::notifyExternalSelectionChanged() { m_controller.notifyExternalSelectionChanged(); }

void RovingListNavHost::doLayout(Renderer& renderer) {
  Flex::doLayout(renderer);
  m_controller.layoutOverlay(width(), height());
}
