#include "core/input/keybind_matcher.h"
#include "render/scene/input_area.h"
#include "ui/controls/input.h"
#include "wayland/wayland_seat.h"

#include <cassert>
#include <string>
#include <xkbcommon/xkbcommon-keysyms.h>

int main() {
  Input input;
  input.setValue("complete metadata value");
  input.setReadOnly(true);
  input.setFrameVisible(false);
  std::string copied;
  input.setOnCopyText([&](std::string value) { copied = std::move(value); });
  int edits = 0;
  input.setOnChange([&](const std::string&) { ++edits; });
  const auto key = [&](std::uint32_t symbol, std::uint32_t utf32 = 0, std::uint32_t modifiers = 0) {
    input.inputArea()->dispatchKey(symbol, utf32, modifiers, true);
  };
  // Read-only Ctrl+A is selection even when the user's editable fields use readline keys.
  Input::setReadlineShortcutsEnabled(true);
  key('a', 0, KeyMod::Ctrl);
  key('c', 0, KeyMod::Ctrl);
  assert(copied == input.value());
  input.selectRange(9, 17);
  key('c', 0, KeyMod::Ctrl);
  assert(copied == "metadata");
  for (const auto symbol : {'x', 'v', 'z', 'y', 'u', 'k', 'w'})
    key(symbol, 0, KeyMod::Ctrl);
  key(XKB_KEY_BackSpace);
  key(XKB_KEY_Delete);
  key('!', '!');
  // A printable Validate binding must never bypass read-only protection.
  Input::setValidateKeyMatcher([](std::uint32_t symbol, std::uint32_t) { return symbol == XKB_KEY_space; });
  key(XKB_KEY_space, ' ');
  TextInputEdit ime;
  ime.hasCommitText = true;
  ime.commitText = "injected";
  ime.hasDelete = true;
  ime.deleteBeforeLength = 5;
  input.textInputApplyEdit(ime);
  assert(input.value() == "complete metadata value" && edits == 0);
  // Shift+arrows retain the existing selection engine; no mouse simulation is needed.
  input.selectRange(0, 0);
  key(XKB_KEY_Right, 0, KeyMod::Shift);
  key('c', 0, KeyMod::Ctrl);
  assert(copied == "c");
  const std::string longValue = "/" + std::string(4096, 'a') + "/full-path";
  input.setValue(longValue);
  input.selectAll();
  key('c', 0, KeyMod::Ctrl);
  assert(copied == longValue);
  input.setReadOnly(false);
  Input::setValidateKeyMatcher({});
  input.setValue("");
  key('x', 'x');
  assert(input.value() == "x" && edits == 1);
  Input::setReadlineShortcutsEnabled(false);
}
