#include "core/input/key_modifiers.h"
#include "core/text_clipboard.h"
#include "render/scene/input_area.h"
#include "tests/test_check.h"
#include "ui/controls/input.h"
#include "ui/controls/stepper.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  class Clipboard final : public TextClipboard {
  public:
    std::optional<std::string> clipboardText() override { return text; }
    void setClipboardText(std::string value) override { text = std::move(value); }
    std::optional<std::string> text;
  };

  void key(Input& input, std::uint32_t sym, std::uint32_t modifiers = 0) {
    input.inputArea()->dispatchKey(sym, sym < 128 ? sym : 0, modifiers, true);
  }

  void commit(Stepper& stepper, const std::string& text) {
    stepper.valueField()->setValue(text);
    key(*stepper.valueField(), XKB_KEY_Return);
  }
} // namespace

int main() {
  Input::setValidateKeyMatcher([](std::uint32_t sym, std::uint32_t modifiers) {
    return sym == XKB_KEY_Return && modifiers == 0;
  });
  Input::setReadlineShortcutsEnabled(false);
  Clipboard clipboard;
  Input::setTextClipboard(&clipboard);
  Stepper stepper;
  stepper.setRange(-100, 100);
  stepper.setValue(7);
  auto& input = *stepper.valueField();
  input.inputArea()->dispatchFocusGain();
  int commits = 0;
  stepper.setOnValueCommitted([&commits](int) { ++commits; });

  if constexpr (std::numeric_limits<long>::max() > std::numeric_limits<int>::max()) {
    commit(stepper, "4294967297");
    TEST_CHECK(stepper.value() == 100);
    commit(stepper, "-4294967297");
    TEST_CHECK(stepper.value() == -100);
    TEST_CHECK(commits == 2);
  }
  commit(stepper, "42");
  const int acceptedCommits = commits;
  commit(stepper, "42");
  TEST_CHECK(commits == acceptedCommits);
  for (const auto* invalid : {"", "abc", "12x", "1.5", "99999999999999999999999999999999999"}) {
    commit(stepper, invalid);
    TEST_CHECK(stepper.value() == 42 && input.value() == "42");
  }
  TEST_CHECK(commits == acceptedCommits);
  stepper.setValueSuffix("px");
  commit(stepper, "  -35 px  ");
  TEST_CHECK(stepper.value() == -35 && input.value() == "-35px");
  stepper.setValueSuffix("");
  stepper.setValue(12);

  key(input, XKB_KEY_a, KeyMod::Ctrl);
  key(input, XKB_KEY_c, KeyMod::Ctrl);
  TEST_CHECK(clipboard.text == "12");
  clipboard.text = "73";
  key(input, XKB_KEY_v, KeyMod::Ctrl);
  TEST_CHECK(input.value() == "73");
  key(input, XKB_KEY_z, KeyMod::Ctrl);
  TEST_CHECK(input.value() == "12");
  key(input, XKB_KEY_y, KeyMod::Ctrl);
  TEST_CHECK(input.value() == "73");
  key(input, XKB_KEY_Return);
  TEST_CHECK(stepper.value() == 73);
  key(input, XKB_KEY_a, KeyMod::Ctrl);
  key(input, XKB_KEY_x, KeyMod::Ctrl);
  TEST_CHECK(input.value().empty() && clipboard.text == "73");
  key(input, XKB_KEY_v, KeyMod::Ctrl);
  TEST_CHECK(input.value() == "73");
  key(input, XKB_KEY_a);
  key(input, XKB_KEY_period);
  TEST_CHECK(input.value() == "73");
  clipboard.text = "not a number";
  key(input, XKB_KEY_a, KeyMod::Ctrl);
  key(input, XKB_KEY_v, KeyMod::Ctrl);
  key(input, XKB_KEY_Return);
  TEST_CHECK(stepper.value() == 73 && input.value() == "73");

  stepper.setEnabled(false);
  commit(stepper, "99");
  TEST_CHECK(stepper.value() == 73);
  stepper.setEnabled(true);
  stepper.setRange(20, -20);
  commit(stepper, "35");
  TEST_CHECK(stepper.value() == 20);
  input.setValue("-9");
  input.inputArea()->dispatchFocusLoss();
  TEST_CHECK(stepper.value() == -9);
  Input::setTextClipboard(nullptr);
  Input::setValidateKeyMatcher({});
  return 0;
}
