#include "core/input/key_modifiers.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/input_area.h"
#include "render/text/cairo_text_renderer.h"
#include "test_check.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/style.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
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
      return TextMetrics{.width = m.width, .left = m.left, .right = m.right, .top = m.top, .bottom = m.bottom};
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
    [[nodiscard]] float renderScale() const noexcept override { return 1.0F; }

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
    TEST_CHECK(label != nullptr && label->parent() != nullptr);
    return *label;
  }

  std::int32_t pressKey(Input& input, std::uint32_t sym, std::uint32_t modifiers = 0) {
    input.inputArea()->dispatchKey(sym, 0, modifiers, true);
    return input.textInputState().cursor;
  }

} // namespace

int main() {
  Style::setRtl(false);
  PangoRenderer renderer;

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

  return 0;
}
