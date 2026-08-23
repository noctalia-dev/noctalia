#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "ui/controls/button.h"
#include "ui/controls/glyph.h"

#include <cmath>
#include <cstdlib>
#include <print>
#include <string_view>
#include <vector>

namespace {

  class StubRenderer final : public Renderer {
  public:
    TextMetrics measureText(
        std::string_view, float fontSize, FontWeight, float, int, TextAlign, std::string_view, TextEllipsize, bool
    ) override {
      return TextMetrics{.bottom = fontSize};
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

  return 0;
}
