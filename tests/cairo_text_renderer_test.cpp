#include "render/text/cairo_text_renderer.h"

#include <array>
#include <cmath>
#include <print>
#include <string_view>

namespace {

  bool sameMetrics(const CairoTextRenderer::TextMetrics& lhs, const CairoTextRenderer::TextMetrics& rhs) {
    constexpr float epsilon = 0.0001F;
    return std::abs(lhs.width - rhs.width) < epsilon
        && std::abs(lhs.top - rhs.top) < epsilon
        && std::abs(lhs.bottom - rhs.bottom) < epsilon
        && lhs.lineCount == rhs.lineCount;
  }

} // namespace

int main() {
  CairoTextRenderer renderer;
  renderer.initialize(nullptr, nullptr);

  constexpr std::string_view text =
      "Error: Error while parsing table header: expected a comment or whitespace, saw 't'";
  constexpr float fontSize = 14.0F;
  const float naturalWidth = renderer.measure(text, fontSize).width;
  constexpr std::array deltas{0.0005F, 0.001F, 0.002F, 0.003F, 0.004F, 0.006F};

  for (const float delta : deltas) {
    renderer.notifyFontConfigChanged();
    const auto above = renderer.measure(text, fontSize, FontWeight::Normal, naturalWidth + delta, 3);
    renderer.notifyFontConfigChanged();
    const auto expectedBelow = renderer.measure(text, fontSize, FontWeight::Normal, naturalWidth - delta, 3);
    if (above.lineCount == expectedBelow.lineCount) {
      continue;
    }

    renderer.notifyFontConfigChanged();
    [[maybe_unused]] const auto cachedAbove =
        renderer.measure(text, fontSize, FontWeight::Normal, naturalWidth + delta, 3);
    const auto cachedBelow = renderer.measure(text, fontSize, FontWeight::Normal, naturalWidth - delta, 3);
    if (!sameMetrics(cachedBelow, expectedBelow)) {
      std::println(
          stderr,
          "cairo_text_renderer_test: FAIL: cached measurement at width {} reused metrics from width {} "
          "(expected {} lines, got {})",
          naturalWidth - delta, naturalWidth + delta, expectedBelow.lineCount, cachedBelow.lineCount
      );
      return 1;
    }
    return 0;
  }

  std::println(stderr, "cairo_text_renderer_test: FAIL: could not establish a wrapping boundary");
  return 1;
}
