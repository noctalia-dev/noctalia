#include "core/input/keybind_matcher.h"
#include "render/scene/input_dispatcher.h"
#include "ui/controls/search_picker.h"

#include <cassert>
#include <string>
#include <xkbcommon/xkbcommon-keysyms.h>

int main() {
  SearchPicker picker;
  picker.setExplicitSelectionMode(true);
  picker.setOptions({{.value = "a", .label = "Alpha"}, {.value = "b", .label = "Beta", .compatible = false}});
  std::string selected;
  picker.setOnSelectionChanged([&](const std::string& value) { selected = value; });
  InputDispatcher dispatcher;
  dispatcher.setSceneRoot(&picker);
  dispatcher.setFocus(picker.filterInputArea());
  assert(selected.empty() && picker.highlightedOption() == nullptr);
  picker.setShowAllApplications(true);
  assert(selected.empty() && picker.highlightedOption() == nullptr);
  dispatcher.keyEvent('b', 'b', 0, true);
  assert(picker.filter() == "b" && selected.empty() && picker.highlightedOption() == nullptr);
  assert(dispatcher.focusedArea() == picker.filterInputArea());
  KeybindMatcher::setMatcher(KeybindAction::Down, [](std::uint32_t sym, std::uint32_t) { return sym == XKB_KEY_Down; });
  dispatcher.keyEvent(XKB_KEY_Down, 0, 0, true);
  assert(selected == "b" && picker.highlightedOption()->value == "b");
  assert(dispatcher.focusedArea() == picker.filterInputArea());
  dispatcher.keyEvent('z', 'z', 0, true);
  assert(selected.empty() && picker.highlightedOption() == nullptr);
  dispatcher.keyEvent(XKB_KEY_BackSpace, 0, 0, true);
  assert(selected.empty() && picker.highlightedOption() == nullptr);
  picker.setSelectedValue("b");
  assert(selected == "b");
  picker.setSelectedValue("");
  assert(selected.empty() && picker.highlightedOption() == nullptr);
}
