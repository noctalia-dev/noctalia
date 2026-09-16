#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "ui/controls/button.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <print>
#include <string_view>
#include <vector>

namespace {

  class StubRenderer final : public Renderer {
  public:
    // Fixed-advance shaping, so a wrap budget maps to an exact character count and the
    // resulting line count is predictable.
    TextMetrics measureText(
        std::string_view text, float fontSize, FontWeight, float maxWidth, int maxLines, TextAlign, std::string_view,
        TextEllipsize, bool
    ) override {
      constexpr float kAdvance = 10.0F;
      const float natural = static_cast<float>(text.size()) * kAdvance;
      float width = natural;
      int lineCount = text.empty() ? 0 : 1;
      if (maxWidth > 0.0F && natural > maxWidth) {
        const float perLine = std::floor(maxWidth / kAdvance) * kAdvance;
        lineCount = perLine > 0.0F ? static_cast<int>(std::ceil(natural / perLine)) : 1;
        if (maxLines > 0) {
          lineCount = std::min(lineCount, maxLines);
        }
        width = maxWidth;
      }
      return TextMetrics{
          .width = width,
          .right = width,
          .bottom = fontSize * static_cast<float>(lineCount),
          .lineCount = lineCount,
      };
    }

    TextMetrics measureFont(float fontSize, FontWeight) override { return TextMetrics{.bottom = fontSize}; }

    void measureTextCursorStops(
        std::string_view, float, const std::vector<std::size_t>&, std::vector<float>&, FontWeight
    ) override {}

    void measureTextCursorStopsWrapped(
        std::string_view, float, const std::vector<std::size_t>&, float, std::vector<TextCursorStop>&, FontWeight
    ) override {}

    TextMetrics measureGlyph(char32_t, float) override {
      return TextMetrics{
          .width = 18.0F,
          .left = 1.5F,
          .right = 19.5F,
          .top = -15.0F,
          .bottom = 3.0F,
          .inkTop = -15.0F,
          .inkBottom = 3.0F,
          .inkLeft = 1.5F,
          .inkRight = 19.5F,
      };
    }

    TextureManager& textureManager() override { std::abort(); }
    [[nodiscard]] float renderScale() const noexcept override { return 1.0F; }
  };

  bool near(float actual, float expected) { return std::abs(actual - expected) < 0.001F; }

} // namespace

int main() {
  StubRenderer renderer;
  Button button;
  button.setGlyph("home");
  button.setGlyphSize(21.0F);
  button.setContentAlign(ButtonContentAlign::Center);
  button.setPadding(4.0F);
  button.setSize(32.0F, 32.0F);
  button.layout(renderer);

  const Glyph* glyph = button.glyph();
  if (glyph == nullptr) {
    std::println(stderr, "button_layout_test: glyph was not created");
    return 1;
  }

  const float glyphCenterX = glyph->x() + glyph->width() * 0.5F;
  const float glyphCenterY = glyph->y() + glyph->height() * 0.5F;
  const float buttonCenterX = button.width() * 0.5F;
  const float buttonCenterY = button.height() * 0.5F;

  if (!near(glyphCenterX, buttonCenterX) || !near(glyphCenterY, buttonCenterY)) {
    std::println(
        stderr, "button_layout_test: centered glyph mismatch: glyph center=({}, {}), button center=({}, {})",
        glyphCenterX, glyphCenterY, buttonCenterX, buttonCenterY
    );
    return 1;
  }

  if (!near(glyph->x(), 5.5F) || !near(glyph->y(), 5.5F)) {
    std::println(
        stderr, "button_layout_test: expected a 21px glyph in a 32px button at (5.5, 5.5), got ({}, {})", glyph->x(),
        glyph->y()
    );
    return 1;
  }

  // A button stretched narrower than its text keeps the label on one line and hands it a
  // budget derived from the assigned box, rather than wrapping inside a fixed-height row.
  Button stretched;
  stretched.setText("a segmented label");
  stretched.setPadding(6.0F);
  stretched.arrange(renderer, LayoutRect{.x = 0.0F, .y = 0.0F, .width = 90.0F, .height = 30.0F});

  const Label* stretchedLabel = stretched.label();
  if (stretchedLabel == nullptr) {
    std::println(stderr, "button_layout_test: stretched button has no label");
    return 1;
  }
  if (!near(stretchedLabel->maxWidth(), 78.0F)) {
    std::println(
        stderr, "button_layout_test: expected a 78px label budget in a 90px button, got {}", stretchedLabel->maxWidth()
    );
    return 1;
  }
  if (stretchedLabel->ellipsize() != TextEllipsize::End) {
    std::println(stderr, "button_layout_test: stretched label does not ellipsize");
    return 1;
  }
  if (stretchedLabel->height() > Style::fontSizeBody * 1.5F) {
    std::println(
        stderr, "button_layout_test: stretched label wrapped instead of ellipsizing (height {})",
        stretchedLabel->height()
    );
    return 1;
  }

  // An unconstrained button must not inherit a budget and must not clip its own text.
  Button intrinsic;
  intrinsic.setText("fits");
  intrinsic.setPadding(6.0F);
  intrinsic.layout(renderer);
  const Label* intrinsicLabel = intrinsic.label();
  if (intrinsicLabel == nullptr || !near(intrinsicLabel->maxWidth(), 0.0F)) {
    std::println(stderr, "button_layout_test: unconstrained button capped its label");
    return 1;
  }

  // A glyph stacked above the label (control-center tiles) leaves the full inner width to the
  // label; only a glyph beside it takes a share.
  Button stacked;
  stacked.setGlyph("home");
  stacked.setGlyphSize(21.0F);
  stacked.setText("Bluetooth");
  stacked.setPadding(6.0F);
  stacked.setDirection(FlexDirection::Vertical);
  stacked.arrange(renderer, LayoutRect{.x = 0.0F, .y = 0.0F, .width = 90.0F, .height = 60.0F});
  if (stacked.label() == nullptr || !near(stacked.label()->maxWidth(), 78.0F)) {
    std::println(
        stderr, "button_layout_test: stacked glyph stole label width, budget {}",
        stacked.label() == nullptr ? -1.0F : stacked.label()->maxWidth()
    );
    return 1;
  }

  Button beside;
  beside.setGlyph("home");
  beside.setGlyphSize(21.0F);
  beside.setText("Bluetooth");
  beside.setPadding(6.0F);
  beside.arrange(renderer, LayoutRect{.x = 0.0F, .y = 0.0F, .width = 90.0F, .height = 30.0F});
  if (beside.label() == nullptr || beside.glyph() == nullptr) {
    std::println(stderr, "button_layout_test: row button missing label or glyph");
    return 1;
  }
  const float besideExpected = std::ceil(78.0F - (beside.glyph()->width() + beside.gap()));
  if (!near(beside.label()->maxWidth(), besideExpected)) {
    std::println(
        stderr, "button_layout_test: expected a {}px budget beside a glyph, got {}", besideExpected,
        beside.label()->maxWidth()
    );
    return 1;
  }

  return 0;
}
