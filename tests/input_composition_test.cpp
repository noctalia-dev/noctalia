#include "core/input/key_modifiers.h"
#include "render/scene/input_area.h"
#include "tests/test_check.h"
#include "ui/controls/input.h"

#include <functional>
#include <memory>
#include <print>
#include <string>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
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
    TEST_CHECK(input.composing());
    cancel(input);
    TEST_CHECK(!input.composing());
    TEST_CHECK(transitions == std::vector<bool>({true, false}));
  }
} // namespace

int main() {
  {
    Input input;
    std::vector<bool> transitions;
    input.setOnCompositionChange([&](bool composing) { transitions.push_back(composing); });
    input.textInputApplyEdit(preedit("first"));
    input.textInputApplyEdit(preedit("updated"));
    TEST_CHECK(transitions == std::vector<bool>({true}));
    TextInputEdit commit;
    commit.hasCommitText = true;
    commit.commitText = "committed";
    input.setOnChange([&](const std::string&) { TEST_CHECK(!input.composing()); });
    input.textInputApplyEdit(commit);
    TEST_CHECK(input.value() == "committed");
    input.textInputResetPreedit();
    TEST_CHECK(transitions == std::vector<bool>({true, false}));
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
    TEST_CHECK(!input);
  }
  {
    auto input = std::make_unique<Input>();
    int losses = 0;
    input->textInputApplyEdit(preedit("pending"));
    input->setOnCompositionChange([&](bool) { input.reset(); });
    input->setOnFocusLoss([&] { ++losses; });
    input->inputArea()->dispatchFocusLoss();
    TEST_CHECK(!input && losses == 0);
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
    TEST_CHECK(!input && changes == 0 && submissions == 0);
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
    TEST_CHECK(!input && submissions == 0);
  }
  {
    Input input;
    input.setValue("a\xc3\xa9\xf0\x9f\x90\x88z");
    input.selectRange(2, 4);
    auto state = input.textInputState();
    TEST_CHECK(state.anchor == 3 && state.cursor == 7);
    input.selectRange(100, 0);
    state = input.textInputState();
    TEST_CHECK(state.anchor == 8 && state.cursor == 0);
    input.selectRange(1, 3);
    input.inputArea()->dispatchKey('x', 'x', 0, true);
    TEST_CHECK(input.value() == "ax\xf0\x9f\x90\x88z");
    input.inputArea()->dispatchKey('z', 'z', KeyMod::Ctrl, true);
    TEST_CHECK(input.value() == "a\xc3\xa9\xf0\x9f\x90\x88z");
  }
  std::println("input composition checks passed");
  return 0;
}
