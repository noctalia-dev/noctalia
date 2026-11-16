#include "core/input/key_modifiers.h"
#include "core/text_clipboard.h"
#include "render/scene/input_area.h"
#include "tests/test_check.h"
#include "ui/controls/input.h"

#include <linux/input-event-codes.h>
#include <optional>
#include <string>
#include <string_view>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  class MockClipboard : public TextClipboard {
  public:
    std::optional<std::string> clipboardText() override { return m_text; }
    void setClipboardText(std::string text) override {
      m_text = std::move(text);
      m_wasSet = true;
    }

    bool wasSet() const noexcept { return m_wasSet; }
    const std::optional<std::string>& text() const noexcept { return m_text; }

  private:
    std::optional<std::string> m_text;
    bool m_wasSet = false;
  };

  void sendKey(Input& input, std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers) {
    input.inputArea()->dispatchKey(sym, utf32, modifiers, true);
  }

  void doubleClickAtStart(Input& input) {
    input.inputArea()->dispatchPress(0.0F, 0.0F, BTN_LEFT, true);
    input.inputArea()->dispatchPress(0.0F, 0.0F, BTN_LEFT, true);
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
  // Ctrl+Backspace: delete previous word (whole field in password mode).
  {
    Input input;
    setup(input, "foo bar", false);
    ctrlBackspace(input);
    TEST_CHECK(input.value() == "foo ");
  }
  {
    Input input;
    setup(input, "foo bar", true);
    ctrlBackspace(input);
    TEST_CHECK(input.value().empty());
  }

  // Ctrl+Delete: delete next word (whole field in password mode).
  {
    Input input;
    setup(input, "foo bar", false);
    pressHome(input);
    ctrlDelete(input);
    TEST_CHECK(input.value() == " bar");
  }
  {
    Input input;
    setup(input, "foo bar", true);
    pressHome(input);
    ctrlDelete(input);
    TEST_CHECK(input.value().empty());
  }

  // Ctrl+Shift+Left: select to previous word start (whole field in password mode).
  {
    Input input;
    setup(input, "foo bar", false);
    ctrlShiftLeft(input);
    pressBackspace(input);
    TEST_CHECK(input.value() == "foo ");
  }
  {
    Input input;
    setup(input, "foo bar", true);
    ctrlShiftLeft(input);
    pressBackspace(input);
    TEST_CHECK(input.value().empty());
  }

  // Ctrl+Shift+Right: select to next word start (whole field in password mode).
  {
    Input input;
    setup(input, "foo bar", false);
    pressHome(input);
    ctrlShiftRight(input);
    pressBackspace(input);
    TEST_CHECK(input.value() == "bar");
  }
  {
    Input input;
    setup(input, "foo bar", true);
    pressHome(input);
    ctrlShiftRight(input);
    pressBackspace(input);
    TEST_CHECK(input.value().empty());
  }

  // Double-click: select word (whole field in password mode).
  {
    Input input;
    setup(input, "foo bar baz", false);
    doubleClickAtStart(input);
    pressBackspace(input);
    TEST_CHECK(input.value() == " bar baz");
  }
  {
    Input input;
    setup(input, "foo bar baz", true);
    doubleClickAtStart(input);
    pressBackspace(input);
    TEST_CHECK(input.value().empty());
  }

  // Ctrl+C: copy the selection, but never in password mode.
  {
    MockClipboard clipboard;
    Input::setTextClipboard(&clipboard);
    Input input;
    setup(input, "foo bar", false);
    ctrlA(input);
    ctrlC(input);
    TEST_CHECK(clipboard.wasSet() && clipboard.text() == "foo bar");
  }
  {
    MockClipboard clipboard;
    Input::setTextClipboard(&clipboard);
    Input input;
    setup(input, "foo bar", true);
    ctrlA(input);
    ctrlC(input);
    TEST_CHECK(!clipboard.wasSet());
  }

  // Ctrl+X: delete the selection; only copies when not in password mode.
  {
    MockClipboard clipboard;
    Input::setTextClipboard(&clipboard);
    Input input;
    setup(input, "foo bar", false);
    ctrlA(input);
    ctrlX(input);
    TEST_CHECK(input.value().empty());
    TEST_CHECK(clipboard.wasSet() && clipboard.text() == "foo bar");
  }
  {
    MockClipboard clipboard;
    Input::setTextClipboard(&clipboard);
    Input input;
    setup(input, "foo bar", true);
    ctrlA(input);
    ctrlX(input);
    TEST_CHECK(input.value().empty());
    TEST_CHECK(!clipboard.wasSet());
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
    TEST_CHECK(input.value() == "secret");
  }
  {
    MockClipboard clipboard;
    Input::setTextClipboard(&clipboard);
    Input input;
    setup(input, "secret", true);
    ctrlA(input);
    ctrlX(input);
    ctrlZ(input);
    TEST_CHECK(input.value().empty());
  }

  // Revealing is display-only: copy, cut, undo and text-input hints keep password semantics.
  {
    MockClipboard clipboard;
    Input::setTextClipboard(&clipboard);
    Input input;
    setup(input, "secret", true);
    input.setPasswordRevealed(true);
    TEST_CHECK(input.passwordRevealed());
    ctrlA(input);
    ctrlC(input);
    TEST_CHECK(!clipboard.wasSet());
    ctrlX(input);
    TEST_CHECK(input.value().empty());
    TEST_CHECK(!clipboard.wasSet());
    ctrlZ(input);
    TEST_CHECK(input.value().empty());
    input.setValue("secret");
    const TextInputState state = input.textInputState();
    TEST_CHECK(state.purpose == TextInputPurpose::Password);
    TEST_CHECK(state.sensitiveData);
    TEST_CHECK(!state.sendSurroundingText && state.surroundingText.empty());
  }

  // Reveal state never outlives password mode, and plain fields cannot be "revealed".
  {
    Input input;
    setup(input, "secret", true);
    input.setPasswordRevealed(true);
    input.setPasswordMode(false);
    input.setPasswordMode(true);
    TEST_CHECK(!input.passwordRevealed());
    input.setPasswordMode(false);
    input.setPasswordRevealed(true);
    TEST_CHECK(!input.passwordRevealed());
  }

  Input::setTextClipboard(nullptr);
  return 0;
}
