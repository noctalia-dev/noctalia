#include "core/input/key_modifiers.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/text/cairo_text_renderer.h"
#include "tests/test_check.h"
#include "ui/controls/input.h"

#include <cstdlib>
#include <functional>
#include <linux/input-event-codes.h>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  class PangoRenderer final : public Renderer {
  public:
    PangoRenderer() { m_text.initialize(nullptr, nullptr); }

    TextMetrics measureText(
        std::string_view text, float fontSize, const TextFont& font, float maxWidth, int maxLines, TextAlign align,
        TextEllipsize ellipsize, bool useMarkup
    ) override {
      const auto m = m_text.measure(1.0F, text, fontSize, font, maxWidth, maxLines, align, ellipsize, useMarkup);
      return TextMetrics{
          .width = m.width,
          .left = m.left,
          .right = m.right,
          .top = m.top,
          .bottom = m.bottom,
          .lineCount = m.lineCount,
          .alignOffsetX = m.alignOffsetX,
      };
    }

    TextMetrics measureFont(float fontSize, const TextFont& font) override {
      const auto m = m_text.measureFont(1.0F, fontSize, font);
      return TextMetrics{.top = m.top, .bottom = m.bottom};
    }

    void measureTextCursorStops(
        std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets,
        std::vector<float>& outStops, const TextFont& font
    ) override {
      m_text.measureCursorStops(1.0F, text, fontSize, byteOffsets, outStops, font);
    }

    void measureTextCursorStopsWrapped(
        std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets, float maxWidth,
        std::vector<TextCursorStop>& outStops, const TextFont& font
    ) override {
      m_text.measureCursorStopsWrapped(1.0F, text, fontSize, byteOffsets, maxWidth, outStops, font);
    }

    void measureTextCharSpans(
        std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets,
        std::vector<TextCharSpan>& outSpans, const TextFont& font
    ) override {
      m_text.measureCharSpans(1.0F, text, fontSize, byteOffsets, outSpans, font);
    }

    TextMetrics measureGlyph(char32_t, float fontSize) override { return TextMetrics{.width = fontSize}; }
    TextureManager& textureManager() override { std::abort(); }
    float renderScale() const noexcept override { return 1.0F; }

  private:
    CairoTextRenderer m_text;
  };

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
  {
    // Preedit over a selection must restore the replaced text and selection direction on cancel,
    // keep surrounding text accurate while composing, and record the pre-composition text for Undo.
    Input input;
    int changes = 0;
    std::string submitted;
    Input::setValidateKeyMatcher([](std::uint32_t sym, std::uint32_t mods) {
      return mods == 0 && (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter);
    });
    input.setOnChange([&](const std::string&) { ++changes; });
    input.setOnSubmit([&](const std::string& value) {
      TEST_CHECK(!input.composing());
      submitted = value;
    });

    // Empty Wayland preedit must not delete an active selection.
    input.setValue("report.txt");
    input.selectRange(0, 6);
    input.textInputApplyEdit(preedit(""));
    TEST_CHECK(input.value() == "report.txt" && !input.composing() && changes == 0);
    auto state = input.textInputState();
    TEST_CHECK(state.anchor == 0 && state.cursor == 6);

    // Wayland preedit update and cancel preserve exact sub-selection (including backward direction),
    // while set_surrounding_text replaces the active preedit with a cursor per text-input-v3.
    input.selectRange(4, 2);
    input.textInputApplyEdit(preedit("a"));
    TEST_CHECK(input.value() == "reart.txt" && input.composing());
    state = input.textInputState();
    TEST_CHECK(state.surroundingText == "rert.txt" && state.anchor == 2 && state.cursor == 2);
    input.textInputApplyEdit(preedit("ab"));
    TEST_CHECK(input.value() == "reabrt.txt");
    input.textInputResetPreedit();
    TEST_CHECK(input.value() == "report.txt" && !input.composing() && changes == 0);
    state = input.textInputState();
    TEST_CHECK(state.surroundingText == "report.txt" && state.anchor == 4 && state.cursor == 2);

    // Surrounding-text deletion before a forward selection preserves forward direction across preedit cancel.
    input.setValue("abcd");
    input.selectRange(1, 3);
    input.textInputApplyEdit(preedit("x"));
    TextInputEdit deleteAndPreedit = preedit("y");
    deleteAndPreedit.hasDelete = true;
    deleteAndPreedit.deleteBeforeLength = 1;
    input.textInputApplyEdit(deleteAndPreedit);
    TEST_CHECK(input.value() == "yd" && input.composing());
    input.textInputResetPreedit();
    TEST_CHECK(input.value() == "bcd" && !input.composing());
    state = input.textInputState();
    TEST_CHECK(state.anchor == 0 && state.cursor == 2);

    // Explicit selectRange() and selectAll() during preedit cancel composition and survive subsequent edits.
    input.setValue("report.txt");
    input.selectRange(0, 6);
    input.textInputApplyEdit(preedit("x"));
    input.selectRange(2, 4);
    TEST_CHECK(input.value() == "report.txt" && !input.composing());
    input.textInputResetPreedit();
    input.inputArea()->dispatchKey('X', 'X', KeyMod::Shift, true, false);
    TEST_CHECK(input.value() == "reXrt.txt");

    input.setValue("report.txt");
    input.selectRange(0, 6);
    input.textInputApplyEdit(preedit("x"));
    input.selectAll();
    state = input.textInputState();
    TEST_CHECK(input.value() == "report.txt" && !input.composing() && state.anchor == 0 && state.cursor == 10);

    // Restore report.txt before the focus-loss, commit, and XKB checks below.
    changes = 0;
    input.setValue("report.txt");

    // Wayland preedit focus loss restores the selection text before onSubmit / onFocusLoss.
    input.selectRange(0, 6);
    input.setSubmitOnFocusLoss(true);
    input.textInputApplyEdit(preedit("tmp"));
    input.inputArea()->dispatchFocusLoss();
    TEST_CHECK(input.value() == "report.txt" && submitted == "report.txt" && changes == 0);
    input.setSubmitOnFocusLoss(false);
    submitted.clear();

    // Wayland IME commit pushes the pre-composition text onto Undo, not the temporary preedit.
    input.selectRange(0, 6);
    input.textInputApplyEdit(preedit("ni"));
    TextInputEdit commit;
    commit.hasCommitText = true;
    commit.commitText = "notes";
    input.textInputApplyEdit(commit);
    TEST_CHECK(input.value() == "notes.txt" && changes == 1);
    input.inputArea()->dispatchKey('z', 'z', KeyMod::Ctrl, true);
    TEST_CHECK(input.value() == "report.txt");

    // XKB dead-key preedit + non-editing keys (Insert, Left, Enter) must not truncate or fire onChange.
    changes = 0;
    input.setValue("report.txt");
    input.selectRange(0, 6);
    input.inputArea()->dispatchKey(XKB_KEY_dead_acute, 0xB4, 0, true, true);
    TEST_CHECK(input.composing());
    input.inputArea()->dispatchKey(XKB_KEY_Insert, 0, 0, true, false);
    TEST_CHECK(input.value() == "report.txt" && !input.composing() && changes == 0);

    input.selectRange(0, 6);
    input.inputArea()->dispatchKey(XKB_KEY_dead_acute, 0xB4, 0, true, true);
    input.inputArea()->dispatchKey(XKB_KEY_Return, '\r', 0, true, false);
    TEST_CHECK(input.value() == "report.txt" && submitted == "report.txt" && changes == 0);

    // XKB dead-key -> composed character -> Ctrl+Z restores the pre-composition text.
    input.setValue("report.txt");
    input.selectRange(0, 6);
    input.inputArea()->dispatchKey(XKB_KEY_dead_acute, 0xB4, 0, true, true);
    input.inputArea()->dispatchKey(XKB_KEY_eacute, 0xE9, 0, true, false);
    TEST_CHECK(input.value() == "\xc3\xa9.txt");
    input.inputArea()->dispatchKey('z', 'z', KeyMod::Ctrl, true, false);
    TEST_CHECK(input.value() == "report.txt");

    // Sub-selection preserved when cancelling XKB dead-key preedit, so next char replaces only that slice.
    input.setValue("report.txt");
    input.selectRange(2, 4);
    input.inputArea()->dispatchKey(XKB_KEY_dead_acute, 0xB4, 0, true, true);
    input.textInputResetPreedit();
    input.inputArea()->dispatchKey('X', 'X', KeyMod::Shift, true, false);
    TEST_CHECK(input.value() == "reXrt.txt");

    // Ctrl+U during preedit over a selection saves the pre-composition value to Undo.
    input.setValue("report.txt");
    input.selectRange(0, 6);
    input.inputArea()->dispatchKey(XKB_KEY_dead_acute, 0xB4, 0, true, true);
    input.inputArea()->dispatchKey('u', 'u', KeyMod::Ctrl, true, false);
    TEST_CHECK(input.value().empty());
    input.inputArea()->dispatchKey('z', 'z', KeyMod::Ctrl, true, false);
    TEST_CHECK(input.value() == "report.txt");
    Input::setValidateKeyMatcher({});
  }
  {
    // Removing preedit must refresh cursor stops before pointer hit-testing and multiline navigation.
    PangoRenderer renderer;

    Input single;
    single.setSize(400.0F, 0.0F);
    single.setValue("report.txt");
    single.selectRange(0, 6);
    single.textInputApplyEdit(preedit("x"));
    single.layout(renderer);
    single.inputArea()->dispatchPress(350.0F, 12.0F, BTN_LEFT, true);
    TEST_CHECK(single.value() == "report.txt" && !single.composing());
    auto state = single.textInputState();
    TEST_CHECK(state.anchor == 10 && state.cursor == 10);

    Input multi;
    multi.setMultiline(true);
    multi.setSize(400.0F, 120.0F);
    multi.setValue("report.txt");
    multi.selectRange(0, 6);
    multi.textInputApplyEdit(preedit("x"));
    multi.layout(renderer);
    multi.inputArea()->dispatchKey(XKB_KEY_End, 0, 0, true, false);
    TEST_CHECK(multi.value() == "report.txt" && !multi.composing());
    state = multi.textInputState();
    TEST_CHECK(state.anchor == 10 && state.cursor == 10);
  }
  std::println("input composition checks passed");
  return 0;
}
