#include "ui/split_pane_focus.h"

#include "core/keybind_matcher.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"

namespace {

  [[nodiscard]] bool isArrowKey(std::uint32_t sym, std::uint32_t modifiers) {
    return KeybindMatcher::matches(KeybindAction::Left, sym, modifiers)
        || KeybindMatcher::matches(KeybindAction::Right, sym, modifiers)
        || KeybindMatcher::matches(KeybindAction::Up, sym, modifiers)
        || KeybindMatcher::matches(KeybindAction::Down, sym, modifiers);
  }

  [[nodiscard]] bool isInSidebar(const InputArea* focused, const SplitPaneFocusConfig& config) {
    if (focused == nullptr || config.sidebarRoot == nullptr) {
      return false;
    }
    if (config.sidebarFocus != nullptr && focused == config.sidebarFocus) {
      return true;
    }
    return isNodeInSubtree(focused, config.sidebarRoot);
  }

  [[nodiscard]] bool isInContent(const InputArea* focused, const SplitPaneFocusConfig& config) {
    if (focused == nullptr || config.contentRoot == nullptr) {
      return false;
    }
    return isNodeInSubtree(focused, config.contentRoot);
  }

  [[nodiscard]] bool isInHeader(const InputArea* focused, const SplitPaneFocusConfig& config) {
    return config.headerFocus != nullptr && focused == config.headerFocus;
  }

  [[nodiscard]] SplitPaneFocusResult
  handleTab(InputDispatcher& dispatcher, const SplitPaneFocusConfig& config, bool reverse) {
    auto focusSidebar = [&]() {
      dispatcher.setFocus(config.sidebarFocus);
      return SplitPaneFocusResult::Consumed;
    };
    auto focusContentFirst = [&]() {
      if (InputArea* area = dispatcher.firstTabFocusUnder(const_cast<Node*>(config.contentRoot))) {
        dispatcher.setFocus(area);
      }
      return SplitPaneFocusResult::Consumed;
    };
    auto focusContentLast = [&]() {
      if (InputArea* area = dispatcher.lastTabFocusUnder(const_cast<Node*>(config.contentRoot))) {
        dispatcher.setFocus(area);
      }
      return SplitPaneFocusResult::Consumed;
    };

    InputArea* const focused = dispatcher.focusedArea();
    if (focused == nullptr || isInHeader(focused, config)) {
      if (reverse) {
        return focusContentLast();
      }
      return focusSidebar();
    }
    if (isInSidebar(focused, config)) {
      if (reverse) {
        return focusContentLast();
      }
      return focusContentFirst();
    }
    if (isInContent(focused, config)) {
      return focusSidebar();
    }
    return SplitPaneFocusResult::NotHandled;
  }

} // namespace

bool isNodeInSubtree(const Node* node, const Node* ancestor) noexcept {
  if (node == nullptr || ancestor == nullptr) {
    return false;
  }
  for (const Node* current = node; current != nullptr; current = current->parent()) {
    if (current == ancestor) {
      return true;
    }
  }
  return false;
}

SplitPaneFocusResult handleSplitPaneFocusNavigation(
    InputDispatcher& dispatcher, const SplitPaneFocusConfig& config, std::uint32_t sym, std::uint32_t modifiers,
    bool pressed, bool preedit
) {
  if (!pressed || preedit) {
    return SplitPaneFocusResult::NotHandled;
  }
  if (config.sidebarFocus == nullptr || config.sidebarRoot == nullptr || config.contentRoot == nullptr) {
    return SplitPaneFocusResult::NotHandled;
  }

  if (KeybindMatcher::matches(KeybindAction::TabPrevious, sym, modifiers)) {
    return handleTab(dispatcher, config, true);
  }
  if (KeybindMatcher::matches(KeybindAction::TabNext, sym, modifiers)) {
    return handleTab(dispatcher, config, false);
  }

  InputArea* const focused = dispatcher.focusedArea();
  if (focused == nullptr) {
    if (!isArrowKey(sym, modifiers)) {
      return SplitPaneFocusResult::NotHandled;
    }
    dispatcher.setFocus(config.sidebarFocus);
    return SplitPaneFocusResult::FocusPrimed;
  }

  if (isInContent(focused, config)) {
    const bool up = KeybindMatcher::matches(KeybindAction::Up, sym, modifiers);
    const bool down = KeybindMatcher::matches(KeybindAction::Down, sym, modifiers);
    if (up && dispatcher.cycleTabFocusInSubtree(const_cast<Node*>(config.contentRoot), true)) {
      return SplitPaneFocusResult::Consumed;
    }
    if (down && dispatcher.cycleTabFocusInSubtree(const_cast<Node*>(config.contentRoot), false)) {
      return SplitPaneFocusResult::Consumed;
    }
  }

  return SplitPaneFocusResult::NotHandled;
}
