#include "core/input/key_modifiers.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/input_area.h"
#include "render/scene/rect_node.h"
#include "render/text/cairo_text_renderer.h"
#include "tests/test_check.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <linux/input-event-codes.h>
#include <string>
#include <string_view>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  // Shapes text with the real Pango renderer so cursor stops carry real bidi reordering.
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
          .alignOffsetX = m.alignOffsetX
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

  constexpr std::string_view kArabic = "السلام عليكم";

  const Label* findLabel(const Node& node) {
    if (const auto* label = dynamic_cast<const Label*>(&node)) {
      return label;
    }
    for (const auto& child : node.children()) {
      if (const Label* found = findLabel(*child)) {
        return found;
      }
    }
    return nullptr;
  }

  const Label& layOut(PangoRenderer& renderer, Input& input, std::string_view value) {
    input.setValue(value);
    input.setSize(400.0F, 0.0F);
    input.layout(renderer);
    const Label* label = findLabel(input);
    TEST_CHECK(label != nullptr);
    TEST_CHECK(label->parent() != nullptr);
    return *label;
  }

  std::int32_t pressKey(Input& input, std::uint32_t sym, std::uint32_t modifiers = 0) {
    input.inputArea()->dispatchKey(sym, 0, modifiers, true);
    return input.textInputState().cursor;
  }

  // Where a multiline input's text is drawn and where its caret, selection and clicks land, in the
  // input's own coordinates.
  struct MultilineView {
    float labelLeft = 0.0F;
    float labelRight = 0.0F;
    float viewportLeft = 0.0F;
    float viewportRight = 0.0F;
    float firstLineY = 0.0F;
  };

  MultilineView layOutMultiline(PangoRenderer& renderer, Input& input, std::string_view value) {
    input.setMultiline(true);
    input.setValue(value);
    input.setSize(400.0F, 200.0F);
    input.layout(renderer);
    const Label* label = findLabel(input);
    TEST_CHECK(label != nullptr);
    TEST_CHECK(label->parent() != nullptr);
    MultilineView view;
    float labelY = 0.0F;
    Node::absolutePosition(label, view.labelLeft, labelY);
    view.labelRight = view.labelLeft + label->width();
    float viewportY = 0.0F;
    Node::absolutePosition(label->parent(), view.viewportLeft, viewportY);
    view.viewportRight = view.viewportLeft + label->parent()->width();
    view.firstLineY = labelY + 4.0F;
    return view;
  }

  float caretX(const Input& input) { return static_cast<float>(input.textInputState().cursorRectX); }

  // The highlighted span of a one-line selection, from the visible selection rects.
  bool selectionSpan(const Input& input, float& left, float& right) {
    bool found = false;
    const Label* label = findLabel(input);
    for (const auto& child : label->parent()->children()) {
      const auto* rect = dynamic_cast<const RectNode*>(child.get());
      if (rect == nullptr || !rect->visible() || rect->width() <= 1.0F) {
        continue;
      }
      float x = 0.0F;
      float y = 0.0F;
      Node::absolutePosition(rect, x, y);
      left = found ? std::min(left, x) : x;
      right = found ? std::max(right, x + rect->width()) : x + rect->width();
      found = true;
    }
    return found;
  }

  std::int32_t clickAt(Input& input, float x, float y) {
    float areaX = 0.0F;
    float areaY = 0.0F;
    Node::absolutePosition(input.inputArea(), areaX, areaY);
    input.inputArea()->dispatchPress(x - areaX, y - areaY, BTN_LEFT, true);
    input.inputArea()->dispatchPress(x - areaX, y - areaY, BTN_LEFT, false);
    return input.textInputState().cursor;
  }

} // namespace

int main() {
  Style::setRtl(false);
  PangoRenderer renderer;

  {
    Input input;
    input.setControlHeight(40.0F);
    input.setClearButtonEnabled(true);
    input.setClearButtonWidth(24.0F);
    layOut(renderer, input, "clearable");
    InputArea* clear = nullptr;
    for (const auto& child : input.children()) {
      auto* area = dynamic_cast<InputArea*>(child.get());
      if (area != nullptr && area != input.inputArea()) {
        clear = area;
      }
    }
    TEST_CHECK(clear != nullptr);
    TEST_CHECK(clear->visible());
    TEST_NEAR(clear->width(), 24.0F, 0.01F);
    TEST_NEAR(clear->x() + clear->width(), input.width(), 0.01F);
    input.setClearButtonWidth(0.0F);
    input.layout(renderer);
    TEST_NEAR(clear->width(), input.height(), 0.01F);
    clear->dispatchPress(1.0F, 1.0F, BTN_LEFT, true);
    clear->dispatchPress(1.0F, 1.0F, BTN_LEFT, false);
    TEST_CHECK(input.value().empty());
  }

  // Latin text in an LTR UI starts at the viewport's left edge, and arrows move one character in
  // byte order.
  {
    Input input;
    const Label& label = layOut(renderer, input, "salam");
    TEST_CHECK(std::abs(label.x()) < 0.5F);
    TEST_CHECK(pressKey(input, XKB_KEY_Left) == 4);
    TEST_CHECK(pressKey(input, XKB_KEY_Right) == 5);
  }

  // Pango lays Arabic out as an RTL paragraph even in an LTR UI. The label sits flush with the
  // viewport's right edge instead of being pushed right by its own width.
  {
    Input input;
    const Label& label = layOut(renderer, input, kArabic);
    const float viewportWidth = label.parent()->width();
    const float right = label.x() + label.width();
    TEST_CHECK(label.x() >= -0.5F);
    TEST_CHECK(right <= viewportWidth + 0.5F);
    TEST_CHECK(viewportWidth - right < 2.0F);
  }

  // Arrows follow the screen: in RTL text Left moves toward the logical end, Right toward the start.
  {
    Input input;
    layOut(renderer, input, kArabic);
    const std::int32_t end = input.textInputState().cursor;
    TEST_CHECK(pressKey(input, XKB_KEY_Left) == end);
    TEST_CHECK(pressKey(input, XKB_KEY_Right) < end);
    TEST_CHECK(pressKey(input, XKB_KEY_Home) == 0);
    TEST_CHECK(pressKey(input, XKB_KEY_Left) > 0);
    TEST_CHECK(pressKey(input, XKB_KEY_Right, KeyMod::Ctrl) == 0);
  }

  // Ctrl+arrows also follow the screen inside an LTR run of an RTL paragraph. Pango draws the run's
  // edges on the paragraph's side: the logical end sits left of "Hello" and its start right of "world".
  {
    Input input;
    layOut(renderer, input, "مرحبا Hello big world");
    const std::int32_t end = input.textInputState().cursor;
    TEST_CHECK(pressKey(input, XKB_KEY_Right, KeyMod::Ctrl) == 17);
    TEST_CHECK(pressKey(input, XKB_KEY_Right, KeyMod::Ctrl) == 21);
    TEST_CHECK(pressKey(input, XKB_KEY_Right, KeyMod::Ctrl) == 11);
    TEST_CHECK(pressKey(input, XKB_KEY_Right, KeyMod::Ctrl) == 0);
    TEST_CHECK(pressKey(input, XKB_KEY_Left, KeyMod::Ctrl) == 11);
    TEST_CHECK(pressKey(input, XKB_KEY_Left, KeyMod::Ctrl) == 21);
    TEST_CHECK(pressKey(input, XKB_KEY_Left, KeyMod::Ctrl) == 17);
    TEST_CHECK(pressKey(input, XKB_KEY_Left, KeyMod::Ctrl) == end);
  }

  // A caret inside a lone LTR word still moves toward the word's edge on the arrow's side.
  {
    Input input;
    layOut(renderer, input, "مرحبا Hello");
    const std::int32_t end = input.textInputState().cursor;
    pressKey(input, XKB_KEY_Right);
    pressKey(input, XKB_KEY_Right);
    TEST_CHECK(pressKey(input, XKB_KEY_Left, KeyMod::Ctrl) == end);
    pressKey(input, XKB_KEY_Right);
    pressKey(input, XKB_KEY_Right);
    TEST_CHECK(pressKey(input, XKB_KEY_Right, KeyMod::Ctrl) == 11);
  }

  // Multiline: an RTL paragraph in an LTR UI is drawn flush with the viewport's right edge, where its
  // caret stops are: the logical start on the right, the end on the left.
  {
    Input input;
    const MultilineView view = layOutMultiline(renderer, input, kArabic);
    TEST_NEAR(view.labelRight, view.viewportRight, 2.0F);
    TEST_CHECK(view.labelLeft > view.viewportLeft + 10.0F);
    pressKey(input, XKB_KEY_Home, KeyMod::Ctrl);
    TEST_NEAR(caretX(input), view.labelRight, 2.0F);
    pressKey(input, XKB_KEY_End, KeyMod::Ctrl);
    TEST_NEAR(caretX(input), view.labelLeft, 2.0F);
  }

  // Multiline LTR text stays at the viewport's left edge.
  {
    Input input;
    const MultilineView view = layOutMultiline(renderer, input, "Hello world");
    TEST_NEAR(view.labelLeft, view.viewportLeft, 2.0F);
    pressKey(input, XKB_KEY_Home, KeyMod::Ctrl);
    TEST_NEAR(caretX(input), view.labelLeft, 2.0F);
    pressKey(input, XKB_KEY_End, KeyMod::Ctrl);
    TEST_NEAR(caretX(input), view.labelRight, 2.0F);
  }

  // Several RTL paragraphs, the longer one second: both end at the right edge, not just the widest.
  {
    Input input;
    const MultilineView view = layOutMultiline(renderer, input, "مرحبا\nالسلام عليكم ورحمة الله");
    TEST_NEAR(view.labelRight, view.viewportRight, 2.0F);
    pressKey(input, XKB_KEY_Home, KeyMod::Ctrl);
    TEST_NEAR(caretX(input), view.viewportRight, 2.0F);
    pressKey(input, XKB_KEY_Down);
    pressKey(input, XKB_KEY_Home);
    TEST_NEAR(caretX(input), view.viewportRight, 2.0F);
  }

  // An RTL paragraph above an LTR one: the block spans the viewport, each paragraph on its own side.
  {
    Input input;
    const MultilineView view = layOutMultiline(renderer, input, std::string(kArabic) + "\nHello world");
    TEST_NEAR(view.labelLeft, view.viewportLeft, 2.0F);
    TEST_NEAR(view.labelRight, view.viewportRight, 2.0F);
    pressKey(input, XKB_KEY_Home, KeyMod::Ctrl);
    TEST_NEAR(caretX(input), view.viewportRight, 2.0F);
    pressKey(input, XKB_KEY_End, KeyMod::Ctrl);
    pressKey(input, XKB_KEY_Home);
    TEST_NEAR(caretX(input), view.viewportLeft, 2.0F);
  }

  // The selection highlight covers the drawn RTL text.
  {
    Input input;
    const MultilineView view = layOutMultiline(renderer, input, kArabic);
    input.selectAll();
    float left = 0.0F;
    float right = 0.0F;
    TEST_CHECK(selectionSpan(input, left, right));
    TEST_NEAR(left, view.labelLeft, 2.0F);
    TEST_NEAR(right, view.labelRight, 2.0F);
  }

  // A click lands on the character drawn under the pointer: the right end of RTL text is its start,
  // the left end its end, and the middle somewhere inside.
  {
    Input input;
    const MultilineView view = layOutMultiline(renderer, input, kArabic);
    const auto end = static_cast<std::int32_t>(kArabic.size());
    TEST_CHECK(clickAt(input, view.labelRight - 1.0F, view.firstLineY) == 0);
    TEST_CHECK(clickAt(input, view.labelLeft + 1.0F, view.firstLineY) == end);
    const std::int32_t middle = clickAt(input, (view.labelLeft + view.labelRight) * 0.5F, view.firstLineY);
    TEST_CHECK(middle > 0);
    TEST_CHECK(middle < end);
  }

  // Mixed directions inside one RTL paragraph keep the same placement.
  {
    Input input;
    constexpr std::string_view kMixed = "اشتريت Yamaha MG10 بسعر 1200";
    const MultilineView view = layOutMultiline(renderer, input, kMixed);
    TEST_NEAR(view.labelRight, view.viewportRight, 2.0F);
    pressKey(input, XKB_KEY_Home, KeyMod::Ctrl);
    TEST_NEAR(caretX(input), view.labelRight, 2.0F);
    const std::int32_t middle = clickAt(input, (view.labelLeft + view.labelRight) * 0.5F, view.firstLineY);
    TEST_CHECK(middle > 0);
    TEST_CHECK(middle < static_cast<std::int32_t>(kMixed.size()));
  }

  return 0;
}
