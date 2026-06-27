#include "core/keybind_matcher.h"

#include "core/key_chord.h"

namespace {

  std::array<std::optional<KeybindMatcher::Matcher>, 8> g_matchers{};

} // namespace

namespace KeybindMatcher {

  void setMatcher(KeybindAction action, Matcher matcher) {
    auto idx = static_cast<std::size_t>(action);
    if (idx < g_matchers.size()) {
      g_matchers[idx] = std::move(matcher);
    }
  }

  bool matches(KeybindAction action, std::uint32_t sym, std::uint32_t modifiers) {
    if (action != KeybindAction::Validate && isPrintableKey(sym) && modifiers == 0) {
      return false;
    }
    auto idx = static_cast<std::size_t>(action);
    if (idx < g_matchers.size() && g_matchers[idx]) {
      return (*g_matchers[idx])(sym, modifiers);
    }
    return false;
  }

} // namespace KeybindMatcher
