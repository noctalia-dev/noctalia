#include "core/input/key_modifiers.h"
#include "render/scene/input_area.h"
#include "tests/test_check.h"
#include "ui/controls/input.h"

#include <string>
#include <string_view>

namespace {
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

  void setup(Input& input, std::string_view value, bool readlineShortcuts) {
    Input::setReadlineShortcutsEnabled(readlineShortcuts);
    input.setValue(value);
  }
} // namespace

int main() {
  // Ctrl+A: move the caret to the start of the field.
  {
    Input input;
    setup(input, "foo bar", true);
    ctrlA(input);
    type(input, 'x');
    TEST_CHECK(input.value() == "xfoo bar");
  }

  // Ctrl+E: move the caret to the end of the field.
  {
    Input input;
    setup(input, "foo bar", true);
    ctrlE(input);
    type(input, 'x');
    TEST_CHECK(input.value() == "foo barx");
  }

  // Ctrl+B: move the caret back one character.
  {
    Input input;
    setup(input, "abc", true);
    ctrlB(input);
    type(input, 'x');
    TEST_CHECK(input.value() == "abxc");
  }

  // Ctrl+F: move the caret forward one character.
  {
    Input input;
    setup(input, "abc", true);
    ctrlA(input);
    ctrlF(input);
    type(input, 'x');
    TEST_CHECK(input.value() == "axbc");
  }

  // Alt+B: move the caret back one word.
  {
    Input input;
    setup(input, "foo bar", true);
    altB(input);
    type(input, 'x');
    TEST_CHECK(input.value() == "foo xbar");
  }

  // Alt+F: move the caret forward one word.
  {
    Input input;
    setup(input, "foo bar", true);
    ctrlA(input);
    altF(input);
    type(input, 'x');
    TEST_CHECK(input.value() == "foox bar");
  }

  // Ctrl+W: delete the word before the caret.
  {
    Input input;
    setup(input, "foo bar", true);
    ctrlW(input);
    TEST_CHECK(input.value() == "foo ");
  }

  // Ctrl+U: clear the whole field.
  {
    Input input;
    setup(input, "foo bar", true);
    ctrlU(input);
    TEST_CHECK(input.value().empty());
  }
  {
    Input input;
    setup(input, "foo bar", true);
    ctrlA(input);
    ctrlF(input);
    ctrlF(input);
    ctrlF(input);
    ctrlU(input);
    // Ctrl+U in the middle kills backward to start.
    TEST_CHECK(input.value() == " bar");
  }

  // Ctrl+K: delete from the caret to the end of the field.
  {
    Input input;
    setup(input, "foo bar", true);
    ctrlA(input);
    ctrlF(input);
    ctrlK(input);
    TEST_CHECK(input.value() == "f");
  }

  // Alt+D: delete the word after the caret.
  {
    Input input;
    setup(input, "foo bar baz", true);
    ctrlA(input);
    altD(input);
    TEST_CHECK(input.value() == " bar baz");
  }

  // With readline shortcuts off, Ctrl+A selects all.
  {
    Input input;
    setup(input, "foo bar", false);
    ctrlA(input);
    type(input, 'x');
    TEST_CHECK(input.value() == "x");
  }

  // With readline shortcuts off, Ctrl+W does not delete words.
  {
    Input input;
    setup(input, "foo bar", false);
    ctrlW(input);
    TEST_CHECK(input.value() == "foo bar");
  }

  return 0;
}
