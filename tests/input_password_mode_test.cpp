#include "core/input/key_modifiers.h"
#include "core/text_clipboard.h"
#include "render/scene/input_area.h"
#include "ui/controls/input.h"

#include <linux/input-event-codes.h>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "input_password_mode_test: {}", message);
    }
    return condition;
  }

  class MockClipboard : public TextClipboard {
  public:
    std::optional<std::string> clipboardText() override { return m_text; }
    void setClipboardText(std::string text) override {
      m_text = std::move(text);
      m_wasSet = true;
    }

    [[nodiscard]] bool wasSet() const noexcept { return m_wasSet; }
    [[nodiscard]] const std::optional<std::string>& text() const noexcept { return m_text; }

  private:
    std::optional<std::string> m_text;
    bool m_wasSet = false;
  };

  void sendKey(Input& input, std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers) {
    input.inputArea()->dispatchKey(sym, utf32, modifiers, true);
  }

  void doubleClickAtStart(Input& input) {
    input.inputArea()->dispatchPress(0.0f, 0.0f, BTN_LEFT, true);
    input.inputArea()->dispatchPress(0.0f, 0.0f, BTN_LEFT, true);
  }

  void pressBackspace(Input& input) { sendKey(input, XKB_KEY_BackSpace, 0, 0); }
  void pressHome(Input& input) { sendKey(input, XKB_KEY_Home, 0, 0); }
  void ctrlBackspace(Input& input) { sendKey(input, XKB_KEY_BackSpace, 0, KeyMod::Ctrl); }
  void ctrlDelete(Input& input) { sendKey(input, XKB_KEY_Delete, 0, KeyMod::Ctrl); }
  void ctrlShiftLeft(Input& input) { sendKey(input, XKB_KEY_Left, 0, KeyMod::Ctrl | KeyMod::Shift); }
  void ctrlShiftRight(Input& input) { sendKey(input, XKB_KEY_Right, 0, KeyMod::Ctrl | KeyMod::Shift); }
  void ctrlA(Input& input) { sendKey(input, 'a', 'a', KeyMod::Ctrl); }
  void ctrlC(Input& input) { sendKey(input, 'c', 'c', KeyMod::Ctrl); }
  void ctrlX(Input& input) { sendKey(input, 'x', 'x', KeyMod::Ctrl); }
  void ctrlZ(Input& input) { sendKey(input, 'z', 'z', KeyMod::Ctrl); }

  void setup(Input& input, std::string_view value, bool passwordMode) {
    if (passwordMode) {
      input.setPasswordMode(true);
    }
    input.setValue(value); // leaves the caret at the end of the text
  }
} // namespace

int main() {
  bool ok = true;

  // Ctrl+Backspace: delete previous word (whole field in password mode).
  {
    Input input;
    setup(input, "foo bar", false);
    ctrlBackspace(input);
    ok = expect(input.value() == "foo ", "non-password Ctrl+Backspace deletes only the last word") && ok;
  }
  {
    Input input;
    setup(input, "foo bar", true);
    ctrlBackspace(input);
    ok = expect(input.value().empty(), "password Ctrl+Backspace deletes the whole field") && ok;
  }

  // Ctrl+Delete: delete next word (whole field in password mode).
  {
    Input input;
    setup(input, "foo bar", false);
    pressHome(input);
    ctrlDelete(input);
    ok = expect(input.value() == " bar", "non-password Ctrl+Delete deletes only the first word") && ok;
  }
  {
    Input input;
    setup(input, "foo bar", true);
    pressHome(input);
    ctrlDelete(input);
    ok = expect(input.value().empty(), "password Ctrl+Delete deletes the whole field") && ok;
  }

  // Ctrl+Shift+Left: select to previous word start (whole field in password mode).
  {
    Input input;
    setup(input, "foo bar", false);
    ctrlShiftLeft(input);
    pressBackspace(input);
    ok = expect(input.value() == "foo ", "non-password Ctrl+Shift+Left selects a single word") && ok;
  }
  {
    Input input;
    setup(input, "foo bar", true);
    ctrlShiftLeft(input);
    pressBackspace(input);
    ok = expect(input.value().empty(), "password Ctrl+Shift+Left selects the whole field") && ok;
  }

  // Ctrl+Shift+Right: select to next word start (whole field in password mode).
  {
    Input input;
    setup(input, "foo bar", false);
    pressHome(input);
    ctrlShiftRight(input);
    pressBackspace(input);
    ok = expect(input.value() == "bar", "non-password Ctrl+Shift+Right selects a single word") && ok;
  }
  {
    Input input;
    setup(input, "foo bar", true);
    pressHome(input);
    ctrlShiftRight(input);
    pressBackspace(input);
    ok = expect(input.value().empty(), "password Ctrl+Shift+Right selects the whole field") && ok;
  }

  // Double-click: select word (whole field in password mode).
  {
    Input input;
    setup(input, "foo bar baz", false);
    doubleClickAtStart(input);
    pressBackspace(input);
    ok = expect(input.value() == " bar baz", "non-password double-click selects a single word") && ok;
  }
  {
    Input input;
    setup(input, "foo bar baz", true);
    doubleClickAtStart(input);
    pressBackspace(input);
    ok = expect(input.value().empty(), "password double-click selects the whole field") && ok;
  }

  // Ctrl+C: copy the selection, but never in password mode.
  {
    MockClipboard clipboard;
    Input::setTextClipboard(&clipboard);
    Input input;
    setup(input, "foo bar", false);
    ctrlA(input);
    ctrlC(input);
    ok = expect(clipboard.wasSet() && clipboard.text() == "foo bar", "non-password Ctrl+C copies the selection") && ok;
  }
  {
    MockClipboard clipboard;
    Input::setTextClipboard(&clipboard);
    Input input;
    setup(input, "foo bar", true);
    ctrlA(input);
    ctrlC(input);
    ok = expect(!clipboard.wasSet(), "password Ctrl+C never writes to the clipboard") && ok;
  }

  // Ctrl+X: delete the selection; only copies when not in password mode.
  {
    MockClipboard clipboard;
    Input::setTextClipboard(&clipboard);
    Input input;
    setup(input, "foo bar", false);
    ctrlA(input);
    ctrlX(input);
    ok = expect(input.value().empty(), "non-password Ctrl+X clears the field") && ok;
    ok = expect(clipboard.wasSet() && clipboard.text() == "foo bar", "non-password Ctrl+X copies the cut text") && ok;
  }
  {
    MockClipboard clipboard;
    Input::setTextClipboard(&clipboard);
    Input input;
    setup(input, "foo bar", true);
    ctrlA(input);
    ctrlX(input);
    ok = expect(input.value().empty(), "password Ctrl+X still clears the field") && ok;
    ok = expect(!clipboard.wasSet(), "password Ctrl+X never writes to the clipboard") && ok;
  }

  // Ctrl+Z: undo works normally, but keeps no history to restore in password mode.
  {
    MockClipboard clipboard;
    Input::setTextClipboard(&clipboard);
    Input input;
    setup(input, "secret", false);
    ctrlA(input);
    ctrlX(input);
    ctrlZ(input);
    ok = expect(input.value() == "secret", "non-password Ctrl+Z restores the cut text") && ok;
  }
  {
    MockClipboard clipboard;
    Input::setTextClipboard(&clipboard);
    Input input;
    setup(input, "secret", true);
    ctrlA(input);
    ctrlX(input);
    ctrlZ(input);
    ok = expect(input.value().empty(), "password Ctrl+Z cannot resurrect a cleared password") && ok;
  }

  Input::setTextClipboard(nullptr);
  return ok ? 0 : 1;
}
