#include "core/input/key_modifiers.h"
#include "core/text_clipboard.h"
#include "render/scene/input_area.h"
#include "ui/controls/input.h"
#include "wayland/text_input_service.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  void require(bool condition, const char* message) {
    if (!condition) {
      throw std::runtime_error(message);
    }
  }

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
    require(fixture.clipboard.requests.size() == 1, "paste must request clipboard text");
    change(*fixture.input);
    const std::string expected = fixture.input->value();
    fixture.clipboard.complete(0, "late");
    require(fixture.input->value() == expected, "stale paste changed the input");
  }
} // namespace

int main() {
  try {
    {
      Fixture fixture;
      fixture.input->selectAll();
      int changes = 0;
      fixture.input->setOnChange([&](const std::string&) { ++changes; });
      paste(*fixture.input);
      require(fixture.clipboard.requests.size() == 1, "paste must request clipboard text");
      require(fixture.input->value() == "original" && changes == 0, "paste changed text before the reply");
      fixture.clipboard.complete(0, "caf\xc3\xa9");
      require(fixture.input->value() == "caf\xc3\xa9" && changes == 1, "paste must replace selection once");
      key(*fixture.input, 'z', 'z', KeyMod::Ctrl);
      require(fixture.input->value() == "original", "paste must undo as one edit");
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
      require(fixture.input->value() == "originalnew", "newer paste must supersede an earlier request");
    }
    {
      Fixture fixture;
      fixture.input->selectAll();
      paste(*fixture.input);
      fixture.clipboard.complete(0, std::nullopt);
      require(fixture.input->value() == "original", "unavailable clipboard must preserve the selection");
    }
    {
      Fixture fixture;
      TextInputEdit edit;
      edit.hasPreedit = true;
      edit.preeditText = "composing";
      fixture.input->textInputApplyEdit(edit);
      paste(*fixture.input);
      require(fixture.input->value() == "original", "paste must cancel existing preedit before waiting");
      fixture.clipboard.complete(0, std::nullopt);
      require(fixture.input->value() == "original", "failed paste must keep preedit cancelled");
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
        require(value == "originalpasted", "change callback value must survive input destruction");
      });
      paste(*fixture.input);
      fixture.clipboard.complete(0, "pasted");
      require(!fixture.input, "paste callback must permit input destruction");
    }
    {
      Fixture fixture;
      key(*fixture.input, XKB_KEY_Insert, 0, KeyMod::Shift);
      fixture.clipboard.complete(0, "shift");
      require(fixture.input->value() == "originalshift", "Shift+Insert must request text too");
    }
    {
      CachedClipboard clipboard;
      Input::setTextClipboard(&clipboard);
      Input input;
      input.inputArea()->dispatchFocusGain();
      paste(input);
      require(input.value() == "cached", "cached providers must still complete inline");
      Input::setTextClipboard(nullptr);
    }
    {
      Fixture fixture;
      fixture.input->setPasswordMode(true);
      fixture.input->selectAll();
      paste(*fixture.input);
      fixture.clipboard.complete(0, "secret");
      key(*fixture.input, 'z', 'z', KeyMod::Ctrl);
      require(fixture.input->value() == "secret", "password paste must not retain undo history");
    }
    std::println("input paste checks passed");
  } catch (const std::exception& error) {
    Input::setTextClipboard(nullptr);
    std::println(stderr, "input_paste_test: {}", error.what());
    return 1;
  }
}
