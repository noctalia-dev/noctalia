#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "ui/controls/label.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <print>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

  class StubRenderer final : public Renderer {
  public:
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

    TextMetrics measureGlyph(char32_t, float) override { return TextMetrics{.width = 10.0F, .right = 10.0F}; }

    TextureManager& textureManager() override { std::abort(); }
    [[nodiscard]] float renderScale() const noexcept override { return 1.0F; }
  };

  bool near(float actual, float expected) { return std::abs(actual - expected) < 0.001F; }

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::println(stderr, "home_weather_line_wrap_test: missing home_tab.cpp path");
    return 1;
  }
  std::ifstream sourceFile(argv[1]);
  if (!sourceFile) {
    std::println(stderr, "home_weather_line_wrap_test: cannot read {}", argv[1]);
    return 1;
  }
  std::ostringstream sourceStream;
  sourceStream << sourceFile.rdbuf();
  const std::string source = sourceStream.str();
  const std::regex weatherBlock(
      R"(if \(m_weatherLine != nullptr\) \{[\s\S]*?m_weatherLine->setMaxLines\(1\);[\s\S]*?m_weatherLine->setEllipsize\(TextEllipsize::End\);)"
  );
  if (!std::regex_search(source, weatherBlock)) {
    std::println(stderr, "home_weather_line_wrap_test: home_tab.cpp weatherLine is not maxLines=1 + End ellipsize");
    return 1;
  }
  if (std::regex_search(source, std::regex(R"(m_weatherLine->setMaxLines\(2\))"))) {
    std::println(stderr, "home_weather_line_wrap_test: home_tab.cpp still sets weatherLine maxLines=2");
    return 1;
  }

  StubRenderer renderer;
  constexpr std::string_view kDisabled = "Weather is disabled in config.";
  constexpr float kFont = 14.0F;
  constexpr float kWrapWidth = 140.0F; // 14 chars at 10px advance; string is 30 chars

  Label wrapping;
  wrapping.setText(kDisabled);
  wrapping.setFontSize(kFont);
  wrapping.setMaxWidth(kWrapWidth);
  wrapping.setMaxLines(2);
  wrapping.measure(renderer);

  if (wrapping.height() <= kFont + 0.001F) {
    std::println(
        stderr, "home_weather_line_wrap_test: maxLines=2 should wrap the disabled string (height {})", wrapping.height()
    );
    return 1;
  }

  Label ellipsized;
  ellipsized.setText(kDisabled);
  ellipsized.setFontSize(kFont);
  ellipsized.setMaxWidth(kWrapWidth);
  ellipsized.setMaxLines(1);
  ellipsized.setEllipsize(TextEllipsize::End);
  ellipsized.measure(renderer);

  if (!near(ellipsized.height(), kFont)) {
    std::println(
        stderr, "home_weather_line_wrap_test: maxLines=1+End should stay one line (height {})", ellipsized.height()
    );
    return 1;
  }
  if (ellipsized.width() > kWrapWidth + 0.001F) {
    std::println(
        stderr, "home_weather_line_wrap_test: ellipsized width {} exceeds cap {}", ellipsized.width(), kWrapWidth
    );
    return 1;
  }

  Label live;
  live.setText("12°C · Cloudy");
  live.setFontSize(kFont);
  live.setMaxWidth(kWrapWidth);
  live.setMaxLines(1);
  live.setEllipsize(TextEllipsize::End);
  live.measure(renderer);
  if (!near(live.height(), kFont)) {
    std::println(stderr, "home_weather_line_wrap_test: short live weather wrapped (height {})", live.height());
    return 1;
  }

  return 0;
}
