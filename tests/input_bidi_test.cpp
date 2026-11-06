#include "core/input/key_modifiers.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/input_area.h"
#include "render/scene/rect_node.h"
#include "render/text/cairo_text_renderer.h"
#include "test_check.h"
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
#include <utility>
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

  // The visible selection rects as (left, right), left to right.
  std::vector<std::pair<float, float>> selectionRects(const Input& input) {
    std::vector<std::pair<float, float>> rects;
    const Label* label = findLabel(input);
    for (const auto& child : label->parent()->children()) {
      const auto* rect = dynamic_cast<const RectNode*>(child.get());
      if (rect == nullptr || !rect->visible() || rect->width() <= 1.0F) {
        continue;
      }
      float x = 0.0F;
      float y = 0.0F;
      Node::absolutePosition(rect, x, y);
      rects.emplace_back(x, x + rect->width());
    }
    std::ranges::sort(rects);
    return rects;
  }

  // The selected byte range, from the state the input reports to text-input clients.
  std::size_t selectedFrom(const Input& input) {
    const TextInputState state = input.textInputState();
    return static_cast<std::size_t>(std::min(state.anchor, state.cursor));
  }

  std::size_t selectedTo(const Input& input) {
    const TextInputState state = input.textInputState();
    return static_cast<std::size_t>(std::max(state.anchor, state.cursor));
  }

  float caretXAt(Input& input, std::size_t byte) {
    input.selectRange(byte, byte);
    return caretX(input);
  }

  void dragFromTo(Input& input, float fromX, float toX, float y) {
    float areaX = 0.0F;
    float areaY = 0.0F;
    Node::absolutePosition(input.inputArea(), areaX, areaY);
    input.inputArea()->dispatchPress(fromX - areaX, y - areaY, BTN_LEFT, true);
    input.inputArea()->dispatchMotion(toX - areaX, y - areaY);
    input.inputArea()->dispatchPress(toX - areaX, y - areaY, BTN_LEFT, false);
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

  // Latin and Arabic in one line come from fonts with different metrics, so their caret stops have
  // different tops and heights; they still form one visual line for clicks, Home/End, Up/Down and the
  // selection highlight.
  {
    Input input;
    constexpr std::string_view kArabicFirst = "مرحبا pax-120 عالم";
    const MultilineView view = layOutMultiline(renderer, input, kArabicFirst);
    const auto end = static_cast<std::int32_t>(kArabicFirst.size());
    TEST_CHECK(clickAt(input, view.labelLeft + 1.0F, view.firstLineY) == end);
    const std::size_t latinStart = kArabicFirst.find("pax");
    const std::int32_t inLatin = clickAt(input, (view.labelLeft + view.labelRight) * 0.5F, view.firstLineY);
    TEST_CHECK(inLatin > static_cast<std::int32_t>(latinStart));
    TEST_CHECK(inLatin < static_cast<std::int32_t>(latinStart + std::string_view("pax-120").size()));
    pressKey(input, XKB_KEY_Home, KeyMod::Ctrl);
    TEST_CHECK(pressKey(input, XKB_KEY_End) == end);
    input.selectAll();
    float left = 0.0F;
    float right = 0.0F;
    TEST_CHECK(selectionSpan(input, left, right));
    TEST_NEAR(left, view.labelLeft, 2.0F);
    TEST_NEAR(right, view.labelRight, 2.0F);
  }
  {
    Input input;
    constexpr std::string_view kLatinFirst = "hello مرحبا";
    const MultilineView view = layOutMultiline(renderer, input, std::string(kLatinFirst) + "\nsecond");
    const auto lineEnd = static_cast<std::int32_t>(kLatinFirst.size());
    TEST_CHECK(clickAt(input, view.labelLeft + 1.0F, view.firstLineY) == 0);
    // The first line is the wider one, so the Arabic word spans from the caret before it (its left edge
    // in this left-to-right line) to the label's right edge.
    const auto arabicStart = static_cast<std::int32_t>(kLatinFirst.find(' ')) + 1;
    const float arabicLeft = caretXAt(input, static_cast<std::size_t>(arabicStart));
    const std::int32_t inArabic = clickAt(input, (arabicLeft + view.labelRight) * 0.5F, view.firstLineY);
    TEST_CHECK(inArabic > arabicStart);
    TEST_CHECK(inArabic < lineEnd);
    pressKey(input, XKB_KEY_Home, KeyMod::Ctrl);
    TEST_CHECK(pressKey(input, XKB_KEY_End) == lineEnd);
    pressKey(input, XKB_KEY_Home, KeyMod::Ctrl);
    TEST_CHECK(pressKey(input, XKB_KEY_Down) > lineEnd);
  }

  // Where two runs meet, two caret slots share one x. A drag that starts on a word's edge anchors in
  // that word, and the highlight covers exactly the selected characters.
  {
    Input input;
    constexpr std::string_view kLatinFirst = "hello مرحبا";
    const MultilineView view = layOutMultiline(renderer, input, std::string(kLatinFirst) + "\nsecond");
    const std::size_t arabicStart = kLatinFirst.find(' ') + 1;
    // The right edge of the first line is the Arabic word's first letter.
    dragFromTo(input, view.labelRight - 1.0F, view.labelRight - 12.0F, view.firstLineY);
    TEST_CHECK(selectedFrom(input) == arabicStart);
    TEST_CHECK(selectedTo(input) > arabicStart);
    TEST_CHECK(selectedTo(input) < kLatinFirst.size());
    float left = 0.0F;
    float right = 0.0F;
    TEST_CHECK(selectionSpan(input, left, right));
    TEST_NEAR(right, view.labelRight, 2.0F);

    // "o م" in byte order is drawn in two pieces: "o " after "hell", and "م" at the line's right end.
    input.selectRange(arabicStart - 2, arabicStart + std::string_view("م").size());
    const auto rects = selectionRects(input);
    TEST_CHECK(rects.size() == 2);
    TEST_NEAR(rects.back().second, view.labelRight, 2.0F);
  }
  {
    Input input;
    constexpr std::string_view kArabicFirst = "مرحبا pax-120 عالم";
    const MultilineView view = layOutMultiline(renderer, input, kArabicFirst);
    const std::size_t latinEnd = kArabicFirst.find(" عالم");
    // Digits share one advance, so the right half of the "0" lies this far past its left edge.
    const float zeroLeft = caretXAt(input, latinEnd - 1);
    const float twoLeft = caretXAt(input, latinEnd - 2);
    const float onZeroRight = zeroLeft + (zeroLeft - twoLeft) * 0.75F;
    dragFromTo(input, onZeroRight, twoLeft + 1.0F, view.firstLineY);
    TEST_CHECK(selectedFrom(input) == latinEnd - 2);
    TEST_CHECK(selectedTo(input) == latinEnd);
    const auto rects = selectionRects(input);
    TEST_CHECK(rects.size() == 1);
    TEST_NEAR(rects.front().first, twoLeft, 1.0F);
  }

  // A letter and its marks form one cluster, and every code point in it reports the cluster's extent: a
  // click on the cluster's far half lands after the whole cluster, never between a letter and its mark.
  {
    Input input;
    constexpr std::string_view kVowelled = "مَرْحَبًا";
    const MultilineView view = layOutMultiline(renderer, input, kVowelled);
    // Meem with its fatha: the caret before it is the cluster's right edge, the caret after it the left.
    const std::size_t afterFirst = std::string_view("مَ").size();
    const float right = caretXAt(input, 0);
    const float left = caretXAt(input, afterFirst);
    TEST_CHECK(right > left);
    TEST_CHECK(clickAt(input, left + (right - left) * 0.25F, view.firstLineY) == static_cast<std::int32_t>(afterFirst));
  }
  {
    Input input;
    // "e" followed by a combining acute accent.
    constexpr std::string_view kDecomposed = "e\u0301te";
    const MultilineView view = layOutMultiline(renderer, input, kDecomposed);
    const std::size_t afterFirst = std::string_view("e\u0301").size();
    const float left = caretXAt(input, 0);
    const float right = caretXAt(input, afterFirst);
    TEST_CHECK(right > left);
    TEST_CHECK(clickAt(input, left + (right - left) * 0.75F, view.firstLineY) == static_cast<std::int32_t>(afterFirst));
  }

  return 0;
}
