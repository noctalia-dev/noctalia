#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

class TextureManager;

enum class TextAlign : std::uint8_t { Start, Center, End };

// Which end of an overflowing single line is replaced with the ellipsis.
// Start keeps the tail (useful for file paths: "…/long/mount/point").
enum class TextEllipsize : std::uint8_t { End, Start, Middle };

enum class FontWeight : int {
  Thin = 100,
  UltraLight = 200,
  Light = 300,
  SemiLight = 350,
  Book = 380,
  Normal = 400,
  Medium = 500,
  SemiBold = 600,
  Bold = 700,
  UltraBold = 800,
  Heavy = 900,
  UltraHeavy = 1000,
};

struct TextMetrics {
  float width = 0.0f;
  float left = 0.0f;
  float right = 0.0f;
  float top = 0.0f;
  float bottom = 0.0f;
  float inkTop = 0.0f;
  float inkBottom = 0.0f;
  float inkLeft = 0.0f;
  float inkRight = 0.0f;
  // Measured baseline-to-cap-top of 'H' for this font/size (0 if unavailable).
  // A stable font property (not per-string ink), used to optically center text
  // by its cap band so caps/digits sit dead-centre. measureFont() populates it.
  float capHeight = 0.0f;
  // Number of laid-out lines for the measured text (0 for empty text). Lets a
  // consumer tell single-line from wrapped text from the measured result rather
  // than re-deriving it from the requested width/line budget.
  int lineCount = 0;
};

class Renderer {
public:
  virtual ~Renderer() = default;

  [[nodiscard]] virtual TextMetrics measureText(
      std::string_view text, float fontSize, FontWeight fontWeight = FontWeight::Normal, float maxWidth = 0.0f,
      int maxLines = 0, TextAlign align = TextAlign::Start, std::string_view fontFamily = {},
      TextEllipsize ellipsize = TextEllipsize::End, bool useMarkup = false
  ) = 0;
  [[nodiscard]] virtual TextMetrics measureFont(float fontSize, FontWeight fontWeight = FontWeight::Normal) = 0;

  // Canonical "as tall as a line of text" row height: the rounded vertical
  // extent of the given font. Bar capsule heights and content widgets that must
  // align with text (e.g. the audio visualizer) size their cross-axis to this
  // instead of re-deriving font metrics by hand. Backed by measureFont(), which
  // is memoized — safe to call from every layout pass.
  [[nodiscard]] virtual float fontRowExtent(float fontSize, FontWeight fontWeight = FontWeight::Normal) {
    const TextMetrics m = measureFont(fontSize, fontWeight);
    return std::round(m.bottom - m.top);
  }
  virtual void measureTextCursorStops(
      std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets, std::vector<float>& outStops,
      FontWeight fontWeight = FontWeight::Normal
  ) = 0;
  [[nodiscard]] virtual TextMetrics measureGlyph(char32_t codepoint, float fontSize) = 0;
  [[nodiscard]] virtual TextureManager& textureManager() = 0;
  [[nodiscard]] virtual float renderScale() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t textMetricsGeneration() const noexcept { return 0; }
  virtual void notifyFontConfigChanged() {}
};
