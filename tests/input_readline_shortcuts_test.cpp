#include "core/input/key_modifiers.h"
#include "render/scene/input_area.h"
#include "ui/controls/input.h"

#include <print>
#include <string>
#include <string_view>

namespace {
  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "input_edit_shortcuts_test: {}", message);
    }
    return condition;
  }

  void sendKey(Input& input, std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers) {
    input.inputArea()->dispatchKey(sym, utf32, modifiers, true);
  }

  void ctrlA(Input& input) { sendKey(input, 'a', 0, KeyMod::Ctrl); }
  void ctrlE(Input& input) { sendKey(input, 'e', 0, KeyMod::Ctrl); }
  void ctrlB(Input& input) { sendKey(input, 'b', 0, KeyMod::Ctrl); }
  void ctrlF(Input& input) { sendKey(input, 'f', 0, KeyMod::Ctrl); }
  void ctrlW(Input& input) { sendKey(input, 'w', 0, KeyMod::Ctrl); }
  void ctrlU(Input& input) { sendKey(input, 'u', 0, KeyMod::Ctrl); }
  void ctrlK(Input& input) { sendKey(input, 'k', 0, KeyMod::Ctrl); }
  void altB(Input& input) { sendKey(input, 'b', 0, KeyMod::Alt); }
  void altF(Input& input) { sendKey(input, 'f', 0, KeyMod::Alt); }
  void altD(Input& input) { sendKey(input, 'd', 0, KeyMod::Alt); }
  void type(Input& input, char c) { sendKey(input, c, c, 0); }

  void setup(Input& input, std::string_view value, bool lineEditing) {
    if (lineEditing) {
      input.setLineEditingEnabled(true);
    }
    input.setValue(value);
  }
} // namespace

int main() {
  bool ok = true;

  // Ctrl+A: move the caret to the start of the field.
  {
    Input input;
    setup(input, "foo bar", true);
    ctrlA(input);
    type(input, 'x');
    ok = expect(input.value() == "xfoo bar", "Ctrl+A puts the caret at the start") && ok;
  }

  // Ctrl+E: move the caret to the end of the field.
  {
    Input input;
    setup(input, "foo bar", true);
    ctrlE(input);
    type(input, 'x');
    ok = expect(input.value() == "foo barx", "Ctrl+E puts the caret at the end") && ok;
  }

  // Ctrl+B: move the caret back one character.
  {
    Input input;
    setup(input, "abc", true);
    ctrlB(input);
    type(input, 'x');
    ok = expect(input.value() == "abxc", "Ctrl+B moves the caret back one character") && ok;
  }

  // Ctrl+F: move the caret forward one character.
  {
    Input input;
    setup(input, "abc", true);
    ctrlA(input);
    ctrlF(input);
    type(input, 'x');
    ok = expect(input.value() == "axbc", "Ctrl+F moves the caret forward one character") && ok;
  }

  // Alt+B: move the caret back one word.
  {
    Input input;
    setup(input, "foo bar", true);
    altB(input);
    type(input, 'x');
    ok = expect(input.value() == "foo xbar", "Alt+B moves the caret back one word") && ok;
  }

  // Alt+F: move the caret forward one word.
  {
    Input input;
    setup(input, "foo bar", true);
    ctrlA(input);
    altF(input);
    type(input, 'x');
    ok = expect(input.value() == "foox bar", "Alt+F moves the caret forward one word") && ok;
  }

  // Ctrl+W: delete the word before the caret.
  {
    Input input;
    setup(input, "foo bar", true);
    ctrlW(input);
    ok = expect(input.value() == "foo ", "Ctrl+W deletes the previous word") && ok;
  }

  // Ctrl+U: clear the whole field.
  {
    Input input;
    setup(input, "foo bar", true);
    ctrlU(input);
    ok = expect(input.value().empty(), "Ctrl+U clears the whole field") && ok;
  }
  {
    Input input;
    setup(input, "foo bar", true);
    ctrlA(input);
    ctrlF(input);
    ctrlF(input);
    ctrlF(input);
    ctrlU(input);
    ok = expect(input.value() == " bar", "Ctrl+U in the middle kills backward to start") && ok;
  }

  // Ctrl+K: delete from the caret to the end of the field.
  {
    Input input;
    setup(input, "foo bar", true);
    ctrlA(input);
    ctrlF(input);
    ctrlK(input);
    ok = expect(input.value() == "f", "Ctrl+K deletes the text after the caret") && ok;
  }

  // Alt+D: delete the word after the caret.
  {
    Input input;
    setup(input, "foo bar baz", true);
    ctrlA(input);
    altD(input);
    ok = expect(input.value() == " bar baz", "Alt+D deletes the next word") && ok;
  }

  // With line editing off, Ctrl+A selects all.
  {
    Input input;
    setup(input, "foo bar", false);
    ctrlA(input);
    type(input, 'x');
    ok = expect(input.value() == "x", "without line editing Ctrl+A stays \"select all\"") && ok;
  }

  // With line editing off, Ctrl+W does not delete words.
  {
    Input input;
    setup(input, "foo bar", false);
    ctrlW(input);
    ok = expect(input.value() == "foo bar", "without line editing Ctrl+W does nothing") && ok;
  }

  return ok ? 0 : 1;
}
