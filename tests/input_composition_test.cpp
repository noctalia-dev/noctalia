#include "core/input/key_modifiers.h"
#include "render/scene/input_area.h"
#include "ui/controls/input.h"

#include <functional>
#include <memory>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  void require(bool value, const char* message) {
    if (!value) {
      throw std::runtime_error(message);
    }
  }
  TextInputEdit preedit(std::string text) {
    TextInputEdit edit;
    edit.hasPreedit = true;
    edit.preeditText = std::move(text);
    return edit;
  }
  void cancelledBy(const std::function<void(Input&)>& cancel) {
    Input input;
    std::vector<bool> transitions;
    input.setOnCompositionChange([&](bool composing) { transitions.push_back(composing); });
    input.textInputApplyEdit(preedit("composing"));
    require(input.composing(), "preedit must activate composition");
    cancel(input);
    require(!input.composing(), "cancellation left active composition");
    require(transitions == std::vector<bool>({true, false}), "cancellation must report one end transition");
  }
} // namespace

int main() {
  try {
    {
      Input input;
      std::vector<bool> transitions;
      input.setOnCompositionChange([&](bool composing) { transitions.push_back(composing); });
      input.textInputApplyEdit(preedit("first"));
      input.textInputApplyEdit(preedit("updated"));
      require(transitions == std::vector<bool>({true}), "preedit updates must not restart composition");
      TextInputEdit commit;
      commit.hasCommitText = true;
      commit.commitText = "committed";
      input.setOnChange([&](const std::string&) {
        require(!input.composing(), "change callback saw old composition");
      });
      input.textInputApplyEdit(commit);
      require(input.value() == "committed", "commit must replace preedit");
      input.textInputResetPreedit();
      require(transitions == std::vector<bool>({true, false}), "commit/reset emitted duplicate transitions");
    }
    cancelledBy([](Input& input) { input.textInputResetPreedit(); });
    cancelledBy([](Input& input) { input.textInputApplyEdit(preedit("")); });
    cancelledBy([](Input& input) { input.setValue("replacement"); });
    cancelledBy([](Input& input) { input.inputArea()->dispatchFocusLoss(); });
    cancelledBy([](Input& input) { input.inputArea()->dispatchKey('x', 'x', 0, true); });
    cancelledBy([](Input& input) { input.inputArea()->dispatchKey('u', 'u', KeyMod::Ctrl, true); });
    cancelledBy([](Input& input) { input.inputArea()->dispatchKey('z', 'z', KeyMod::Ctrl, true); });
    {
      auto input = std::make_unique<Input>();
      input->setValue("initial");
      input->inputArea()->dispatchKey('x', 'x', 0, true);
      input->textInputApplyEdit(preedit("pending"));
      input->setOnCompositionChange([&](bool composing) {
        if (!composing) {
          input.reset();
        }
      });
      input->inputArea()->dispatchKey('z', 'z', KeyMod::Ctrl, true);
      require(!input, "undo must permit composition-callback destruction");
    }
    {
      auto input = std::make_unique<Input>();
      int losses = 0;
      input->textInputApplyEdit(preedit("pending"));
      input->setOnCompositionChange([&](bool) { input.reset(); });
      input->setOnFocusLoss([&] { ++losses; });
      input->inputArea()->dispatchFocusLoss();
      require(!input && losses == 0, "composition destruction must stop later blur callbacks");
    }
    {
      auto input = std::make_unique<Input>();
      int changes = 0, submissions = 0;
      input->setOnCompositionChange([&](bool) { input.reset(); });
      input->setOnChange([&](const std::string&) { ++changes; });
      input->setOnSubmit([&](const std::string&) { ++submissions; });
      auto edit = preedit("pending");
      edit.hasCommitText = true;
      edit.commitText = "committed";
      edit.submit = true;
      input->textInputApplyEdit(edit);
      require(!input && changes == 0 && submissions == 0, "composition callback destruction must stop other callbacks");
    }
    {
      auto input = std::make_unique<Input>();
      int submissions = 0;
      input->setOnChange([&](const std::string&) { input.reset(); });
      input->setOnSubmit([&](const std::string&) { ++submissions; });
      TextInputEdit edit;
      edit.hasCommitText = true;
      edit.commitText = "committed";
      edit.submit = true;
      input->textInputApplyEdit(edit);
      require(!input && submissions == 0, "edit callback destruction must stop submission");
    }
    {
      Input input;
      input.setValue("a\xc3\xa9\xf0\x9f\x90\x88z");
      input.selectRange(2, 4);
      auto state = input.textInputState();
      require(state.anchor == 3 && state.cursor == 7, "range must advance offsets inside UTF-8 characters");
      input.selectRange(100, 0);
      state = input.textInputState();
      require(state.anchor == 8 && state.cursor == 0, "range must clamp bounds and preserve direction");
      input.selectRange(1, 3);
      input.inputArea()->dispatchKey('x', 'x', 0, true);
      require(input.value() == "ax\xf0\x9f\x90\x88z", "range replacement damaged adjacent UTF-8 text");
      input.inputArea()->dispatchKey('z', 'z', KeyMod::Ctrl, true);
      require(input.value() == "a\xc3\xa9\xf0\x9f\x90\x88z", "range replacement must undo normally");
    }
    std::println("input composition checks passed");
  } catch (const std::exception& error) {
    std::println(stderr, "input_composition_test: {}", error.what());
    return 1;
  }
}
