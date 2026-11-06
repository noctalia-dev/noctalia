#include "core/input/key_modifiers.h"
#include "core/text_clipboard.h"
#include "render/scene/input_area.h"
#include "ui/controls/input.h"
#include "ui/controls/stepper.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  void require(bool condition, const char* message) {
    if (!condition) {
      throw std::runtime_error(message);
    }
  }

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
  try {
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
      require(stepper.value() == 100, "positive wide value wrapped before clamping");
      commit(stepper, "-4294967297");
      require(stepper.value() == -100, "negative wide value wrapped before clamping");
      require(commits == 2, "clamped edits must commit once each");
    }
    commit(stepper, "42");
    const int acceptedCommits = commits;
    commit(stepper, "42");
    require(commits == acceptedCommits, "unchanged value emitted a commit");
    for (const auto* invalid : {"", "abc", "12x", "1.5", "99999999999999999999999999999999999"}) {
      commit(stepper, invalid);
      require(stepper.value() == 42 && input.value() == "42", "invalid edit changed the value or stayed visible");
    }
    require(commits == acceptedCommits, "invalid edits emitted commits");
    stepper.setValueSuffix("px");
    commit(stepper, "  -35 px  ");
    require(stepper.value() == -35 && input.value() == "-35px", "suffix/whitespace commit failed");
    stepper.setValueSuffix("");
    stepper.setValue(12);

    key(input, XKB_KEY_a, KeyMod::Ctrl);
    key(input, XKB_KEY_c, KeyMod::Ctrl);
    require(clipboard.text == "12", "Ctrl+A/C did not copy the numeric selection");
    clipboard.text = "73";
    key(input, XKB_KEY_v, KeyMod::Ctrl);
    require(input.value() == "73", "Ctrl+V did not replace the selection");
    key(input, XKB_KEY_z, KeyMod::Ctrl);
    require(input.value() == "12", "Ctrl+Z did not undo paste");
    key(input, XKB_KEY_y, KeyMod::Ctrl);
    require(input.value() == "73", "Ctrl+Y did not redo paste");
    key(input, XKB_KEY_Return);
    require(stepper.value() == 73, "pasted numeric text did not commit");
    key(input, XKB_KEY_a, KeyMod::Ctrl);
    key(input, XKB_KEY_x, KeyMod::Ctrl);
    require(input.value().empty() && clipboard.text == "73", "Ctrl+X did not cut the value");
    key(input, XKB_KEY_v, KeyMod::Ctrl);
    require(input.value() == "73", "cut text did not paste back");
    key(input, XKB_KEY_a);
    key(input, XKB_KEY_period);
    require(input.value() == "73", "plain nonnumeric keys bypassed the filter");
    clipboard.text = "not a number";
    key(input, XKB_KEY_a, KeyMod::Ctrl);
    key(input, XKB_KEY_v, KeyMod::Ctrl);
    key(input, XKB_KEY_Return);
    require(stepper.value() == 73 && input.value() == "73", "invalid clipboard text was accepted");

    stepper.setEnabled(false);
    commit(stepper, "99");
    require(stepper.value() == 73, "disabled stepper committed an edit");
    stepper.setEnabled(true);
    stepper.setRange(20, -20);
    commit(stepper, "35");
    require(stepper.value() == 20, "reversed range was not clamped");
    input.setValue("-9");
    input.inputArea()->dispatchFocusLoss();
    require(stepper.value() == -9, "focus-loss edit did not commit");
    Input::setTextClipboard(nullptr);
    Input::setValidateKeyMatcher({});
  } catch (const std::exception& error) {
    Input::setTextClipboard(nullptr);
    Input::setValidateKeyMatcher({});
    std::println(stderr, "stepper_input_test: {}", error.what());
    return 1;
  }
  return 0;
}
