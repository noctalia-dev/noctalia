#include "core/input/key_modifiers.h"
#include "core/text_clipboard.h"
#include "render/scene/input_area.h"
#include "tests/test_check.h"
#include "ui/controls/input.h"
#include "wayland/text_input_service.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  class CachedClipboard : public TextClipboard {
  public:
    std::optional<std::string> clipboardText() override { return "cached"; }
    void setClipboardText(std::string) override {}
  };

  class DeferredClipboard final : public CachedClipboard {
  public:
    void requestClipboardText(std::function<void(std::optional<std::string>)> callback) override {
      requests.push_back(std::move(callback));
    }
    void complete(std::size_t index, std::optional<std::string> text) {
      auto callback = std::move(requests.at(index));
      callback(std::move(text));
    }
    std::vector<std::function<void(std::optional<std::string>)>> requests;
  };

  struct Fixture {
    Fixture() {
      Input::setTextClipboard(&clipboard);
      input->setValue("original");
      input->inputArea()->dispatchFocusGain();
    }
    ~Fixture() { Input::setTextClipboard(nullptr); }

    DeferredClipboard clipboard;
    std::unique_ptr<Input> input = std::make_unique<Input>();
  };

  void key(Input& input, std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers) {
    input.inputArea()->dispatchKey(sym, utf32, modifiers, true);
  }
  void paste(Input& input) { key(input, 'v', 'v', KeyMod::Ctrl); }

  void staleReply(const std::function<void(Input&)>& change) {
    Fixture fixture;
    paste(*fixture.input);
    TEST_CHECK(fixture.clipboard.requests.size() == 1);
    change(*fixture.input);
    const std::string expected = fixture.input->value();
    fixture.clipboard.complete(0, "late");
    TEST_CHECK(fixture.input->value() == expected);
  }
} // namespace

int main() {
  {
    Fixture fixture;
    fixture.input->selectAll();
    int changes = 0;
    fixture.input->setOnChange([&](const std::string&) { ++changes; });
    paste(*fixture.input);
    TEST_CHECK(fixture.clipboard.requests.size() == 1);
    TEST_CHECK(fixture.input->value() == "original" && changes == 0);
    fixture.clipboard.complete(0, "caf\xc3\xa9");
    TEST_CHECK(fixture.input->value() == "caf\xc3\xa9" && changes == 1);
    key(*fixture.input, 'z', 'z', KeyMod::Ctrl);
    TEST_CHECK(fixture.input->value() == "original");
  }
  staleReply([](Input& input) { key(input, 'x', 'x', 0); });
  staleReply([](Input& input) { input.setValue("replaced"); });
  staleReply([](Input& input) { input.moveCaretLeft(); });
  staleReply([](Input& input) { input.selectAll(); });
  staleReply([](Input& input) { input.inputArea()->dispatchFocusLoss(); });
  staleReply([](Input& input) {
    input.inputArea()->dispatchFocusLoss();
    input.inputArea()->dispatchFocusGain();
  });
  staleReply([](Input& input) {
    input.setEnabled(false);
    input.setEnabled(true);
  });
  staleReply([](Input& input) {
    TextInputService service;
    input.textInputActivated(service);
    input.textInputDeactivated(service);
  });
  staleReply([](Input& input) {
    TextInputEdit edit;
    edit.hasPreedit = true;
    edit.preeditText = "composing";
    input.textInputApplyEdit(edit);
  });
  staleReply([](Input& input) {
    TextInputEdit edit;
    edit.hasPreedit = true;
    edit.preeditText = "composing";
    input.textInputApplyEdit(edit);
    input.textInputResetPreedit();
  });
  staleReply([](Input& input) {
    TextInputEdit edit;
    edit.hasCommitText = true;
    edit.commitText = "committed";
    input.textInputApplyEdit(edit);
  });
  {
    Fixture fixture;
    paste(*fixture.input);
    paste(*fixture.input);
    fixture.clipboard.complete(1, "new");
    fixture.clipboard.complete(0, "old");
    TEST_CHECK(fixture.input->value() == "originalnew");
  }
  {
    Fixture fixture;
    fixture.input->selectAll();
    paste(*fixture.input);
    fixture.clipboard.complete(0, std::nullopt);
    TEST_CHECK(fixture.input->value() == "original");
  }
  {
    Fixture fixture;
    TextInputEdit edit;
    edit.hasPreedit = true;
    edit.preeditText = "composing";
    fixture.input->textInputApplyEdit(edit);
    paste(*fixture.input);
    TEST_CHECK(fixture.input->value() == "original");
    fixture.clipboard.complete(0, std::nullopt);
    TEST_CHECK(fixture.input->value() == "original");
  }
  {
    Fixture fixture;
    paste(*fixture.input);
    fixture.input.reset();
    fixture.clipboard.complete(0, "late");
  }
  {
    Fixture fixture;
    fixture.input->setOnChange([&](const std::string& value) {
      fixture.input.reset();
      TEST_CHECK(value == "originalpasted");
    });
    paste(*fixture.input);
    fixture.clipboard.complete(0, "pasted");
    TEST_CHECK(!fixture.input);
  }
  {
    Fixture fixture;
    key(*fixture.input, XKB_KEY_Insert, 0, KeyMod::Shift);
    fixture.clipboard.complete(0, "shift");
    TEST_CHECK(fixture.input->value() == "originalshift");
  }
  {
    CachedClipboard clipboard;
    Input::setTextClipboard(&clipboard);
    Input input;
    input.inputArea()->dispatchFocusGain();
    paste(input);
    TEST_CHECK(input.value() == "cached");
    Input::setTextClipboard(nullptr);
  }
  {
    Fixture fixture;
    fixture.input->setPasswordMode(true);
    fixture.input->selectAll();
    paste(*fixture.input);
    fixture.clipboard.complete(0, "secret");
    key(*fixture.input, 'z', 'z', KeyMod::Ctrl);
    TEST_CHECK(fixture.input->value() == "secret");
  }
  std::println("input paste checks passed");
  return 0;
}
