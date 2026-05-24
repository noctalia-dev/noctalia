#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

class TextureManager;

enum class TextAlign : std::uint8_t { Start, Center, End };

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
};

class Renderer {
public:
  virtual ~Renderer() = default;

  [[nodiscard]] virtual TextMetrics measureText(
      std::string_view text, float fontSize, FontWeight fontWeight = FontWeight::Normal, float maxWidth = 0.0f,
      int maxLines = 0, TextAlign align = TextAlign::Start, std::string_view fontFamily = {}
  ) = 0;
  [[nodiscard]] virtual TextMetrics measureFont(float fontSize, FontWeight fontWeight = FontWeight::Normal) = 0;
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
