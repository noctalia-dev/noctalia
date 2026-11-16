#include "core/input/keybind_matcher.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "ui/controls/input.h"
#include "wayland/wayland_seat.h"

#include <cassert>
#include <memory>
#include <string>
#include <xkbcommon/xkbcommon-keysyms.h>

int main() {
  Node root;
  auto inputNode = std::make_unique<Input>();
  auto* input = inputNode.get();
  root.addChild(std::move(inputNode));
  InputDispatcher dispatcher;
  dispatcher.setSceneRoot(&root);
  input->setSize(180.0F, 32.0F);
  input->inputArea()->setFrameSize(180.0F, 32.0F);
  input->setValue("complete metadata value");
  input->setReadOnly(true);
  input->setFrameVisible(false);
  std::string copied;
  input->setOnCopyText([&](std::string value) { copied = std::move(value); });
  int edits = 0;
  input->setOnChange([&](const std::string&) { ++edits; });
  const auto key = [&](std::uint32_t symbol, std::uint32_t utf32 = 0, std::uint32_t modifiers = 0) {
    dispatcher.keyEvent(symbol, utf32, modifiers, true);
  };
  // A completed pointer selection leaves read-only metadata focused for the subsequent Ctrl+C.
  (void)dispatcher.pointerButton(10.0F, 10.0F, BTN_LEFT, true, 1, 1, false);
  input->selectRange(9, 17);
  (void)dispatcher.pointerButton(10.0F, 10.0F, BTN_LEFT, false, 2, 2, false);
  assert(dispatcher.focusedArea() == input->inputArea());
  key('c', 0, KeyMod::Ctrl);
  assert(copied == "metadata");
  // Read-only Ctrl+A is selection even when the user's editable fields use readline keys.
  Input::setReadlineShortcutsEnabled(true);
  key('a', 0, KeyMod::Ctrl);
  key('c', 0, KeyMod::Ctrl);
  assert(copied == input->value());
  input->selectRange(9, 17);
  key('c', 0, KeyMod::Ctrl);
  assert(copied == "metadata");
  input->setValue("path: /tmp/naïve-猫");
  input->selectRange(11, input->value().size());
  key('c', 0, KeyMod::Ctrl);
  assert(copied == "naïve-猫");
  assert(input->hasTextSelection());
  assert(input->copySelectedText());
  assert(copied == "naïve-猫" && input->hasTextSelection());
  input->setValue("complete metadata value");
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
  input->textInputApplyEdit(ime);
  assert(input->value() == "complete metadata value" && edits == 0);
  // Shift+arrows retain the existing selection engine; no mouse simulation is needed.
  input->selectRange(0, 0);
  key(XKB_KEY_Right, 0, KeyMod::Shift);
  key('c', 0, KeyMod::Ctrl);
  assert(copied == "c");
  const std::string longValue = "/" + std::string(4096, 'a') + "/full-path";
  input->setValue(longValue);
  input->selectAll();
  key('c', 0, KeyMod::Ctrl);
  assert(copied == longValue);
  input->setReadOnly(false);
  dispatcher.setFocus(nullptr);
  (void)dispatcher.pointerButton(10.0F, 10.0F, BTN_LEFT, true, 3, 3, false);
  (void)dispatcher.pointerButton(10.0F, 10.0F, BTN_LEFT, false, 4, 4, false);
  assert(dispatcher.focusedArea() == input->inputArea());
  Input::setValidateKeyMatcher({});
  input->setValue("");
  key('x', 'x');
  assert(input->value() == "x" && edits == 1);
  Input::setReadlineShortcutsEnabled(false);
}
