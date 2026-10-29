#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
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

// Font selection for a run of text. An empty family means the renderer-global
// family (shell.font_family). `features` is an OpenType feature list in Pango's
// font-features syntax (e.g. "tnum"); empty applies the font defaults.
struct TextFont {
  FontWeight weight = FontWeight::Normal;
  std::string family;
  std::string features;

  bool operator==(const TextFont&) const = default;
};

// Caret geometry for one byte offset in a laid-out text: caret x, top and
// height in logical px from the layout origin. The wrapped variant of the
// cursor-stops query fills these so editors can place carets and selections
// in multi-line text.
struct TextCursorStop {
  float x = 0.0F;
  float y = 0.0F;
  float height = 0.0F;
};

struct TextCharSpan {
  float left = 0.0F;
  float right = 0.0F;
};

struct TextMetrics {
  float width = 0.0F;
  float left = 0.0F;
  float right = 0.0F;
  float top = 0.0F;
  float bottom = 0.0F;
  float inkTop = 0.0F;
  float inkBottom = 0.0F;
  float inkLeft = 0.0F;
  float inkRight = 0.0F;
  // Measured baseline-to-cap-top of 'H' for this font/size (0 if unavailable).
  // A stable font property (not per-string ink), used to optically center text
  // by its cap band so caps/digits sit dead-centre. measureFont() populates it.
  float capHeight = 0.0F;
  // Number of laid-out lines for the measured text (0 for empty text). Lets a
  // consumer tell single-line from wrapped text from the measured result rather
  // than re-deriving it from the requested width/line budget.
  int lineCount = 0;
  // Left edge of the leftmost line within maxWidth for wrapped text. Drawn text starts at the draw x, so a
  // caller that needs each line where Pango placed it (e.g. right-aligned RTL paragraphs) offsets by this.
  float alignOffsetX = 0.0F;
};

class Renderer {
public:
  virtual ~Renderer() = default;

  [[nodiscard]] virtual TextMetrics measureText(
      std::string_view text, float fontSize, const TextFont& font = {}, float maxWidth = 0.0F, int maxLines = 0,
      TextAlign align = TextAlign::Start, TextEllipsize ellipsize = TextEllipsize::End, bool useMarkup = false
  ) = 0;
  // Metrics of `font` at the given size.
  [[nodiscard]] virtual TextMetrics measureFont(float fontSize, const TextFont& font = {}) = 0;

  // Canonical "as tall as a line of text" row height: the rounded vertical
  // extent of the given font. Bar capsule heights and content widgets that must
  // align with text (e.g. the audio visualizer) size their cross-axis to this
  // instead of re-deriving font metrics by hand. Backed by measureFont(), which
  // is memoized — safe to call from every layout pass.
  [[nodiscard]] virtual float fontRowExtent(float fontSize, const TextFont& font = {}) {
    const TextMetrics m = measureFont(fontSize, font);
    return std::round(m.bottom - m.top);
  }
  virtual void measureTextCursorStops(
      std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets, std::vector<float>& outStops,
      const TextFont& font = {}
  ) = 0;
  // Like measureTextCursorStops, but lays the text out wrapped at maxWidth
  // (word-char wrap, '\n' honored — the same layout the draw path uses for a
  // maxLines=0 text node) and reports full caret rects instead of x only.
  virtual void measureTextCursorStopsWrapped(
      std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets, float maxWidth,
      std::vector<TextCursorStop>& outStops, const TextFont& font = {}
  ) = 0;
  // Horizontal extent of each character on screen: span i covers the bytes from byteOffsets[i] to
  // byteOffsets[i + 1], laid out on one line like measureTextCursorStops. Right-to-left runs put the
  // spans out of byte order. Leaves outSpans empty when the renderer cannot shape text.
  virtual void measureTextCharSpans(
      std::string_view /*text*/, float /*fontSize*/, const std::vector<std::size_t>& /*byteOffsets*/,
      std::vector<TextCharSpan>& outSpans, const TextFont& /*font*/ = {}
  ) {
    outSpans.clear();
  }
  [[nodiscard]] virtual TextMetrics measureGlyph(char32_t codepoint, float fontSize) = 0;
  [[nodiscard]] virtual TextureManager& textureManager() = 0;
  [[nodiscard]] virtual float renderScale() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t textMetricsGeneration() const noexcept { return 0; }
  virtual void notifyFontConfigChanged() {}
};
