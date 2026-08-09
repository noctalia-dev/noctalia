#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

// RGBA color; every channel is a float in [0,1].
struct Color {
  float r = 0.0F;
  float g = 0.0F;
  float b = 0.0F;
  float a = 1.0F;
};

constexpr bool operator==(const Color& lhs, const Color& rhs) noexcept {
  return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b && lhs.a == rhs.a;
}

constexpr Color rgba(float r, float g, float b, float a = 1.0F) { return Color{r, g, b, a}; }

// Maps a 0-255 channel byte to a [0,1] float.
constexpr float colorByte(std::uint32_t value) { return static_cast<float>(value) / 255.0F; }

// Builds a Color from a packed 0xRRGGBB integer (alpha = 1).
constexpr Color rgbHex(std::uint32_t value) {
  return Color{
      .r = colorByte((value >> 16U) & 0xFFU),
      .g = colorByte((value >> 8U) & 0xFFU),
      .b = colorByte(value & 0xFFU),
      .a = 1.0F,
  };
}

// Builds a Color from a packed 0xRRGGBBAA integer.
constexpr Color rgbaHex(std::uint32_t value) {
  return Color{
      .r = colorByte((value >> 24U) & 0xFFU),
      .g = colorByte((value >> 16U) & 0xFFU),
      .b = colorByte((value >> 8U) & 0xFFU),
      .a = colorByte(value & 0xFFU),
  };
}

// One hex digit ('0'-'9', 'a'-'f', 'A'-'F') to its value; throws on anything else.
constexpr std::uint32_t hexDigit(char c) {
  if (c >= '0' && c <= '9') {
    return static_cast<std::uint32_t>(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return static_cast<std::uint32_t>(10 + (c - 'a'));
  }
  if (c >= 'A' && c <= 'F') {
    return static_cast<std::uint32_t>(10 + (c - 'A'));
  }
  throw std::invalid_argument("invalid hex digit");
}

constexpr std::uint32_t hexByte(char high, char low) { return (hexDigit(high) << 4U) | hexDigit(low); }

// Parses "#rgb", "#rgba", "#rrggbb", or "#rrggbbaa" (literal '#' required); throws otherwise.
constexpr Color hex(std::string_view value) {
  if (value.empty() || value.front() != '#') {
    throw std::invalid_argument("hex color must start with '#'");
  }

  if (value.size() == 4) {
    return Color{
        .r = colorByte(hexDigit(value[1]) * 17U),
        .g = colorByte(hexDigit(value[2]) * 17U),
        .b = colorByte(hexDigit(value[3]) * 17U),
        .a = 1.0F,
    };
  }

  if (value.size() == 5) {
    return Color{
        .r = colorByte(hexDigit(value[1]) * 17U),
        .g = colorByte(hexDigit(value[2]) * 17U),
        .b = colorByte(hexDigit(value[3]) * 17U),
        .a = colorByte(hexDigit(value[4]) * 17U),
    };
  }

  if (value.size() == 7) {
    return Color{
        .r = colorByte(hexByte(value[1], value[2])),
        .g = colorByte(hexByte(value[3], value[4])),
        .b = colorByte(hexByte(value[5], value[6])),
        .a = 1.0F,
    };
  }

  if (value.size() == 9) {
    return Color{
        .r = colorByte(hexByte(value[1], value[2])),
        .g = colorByte(hexByte(value[3], value[4])),
        .b = colorByte(hexByte(value[5], value[6])),
        .a = colorByte(hexByte(value[7], value[8])),
    };
  }

  throw std::invalid_argument("unsupported hex color format");
}

constexpr Color withAlpha(const Color& color, float alpha) {
  auto clamp = [](float v) constexpr { return v < 0.0F ? 0.0F : (v > 1.0F ? 1.0F : v); };
  return rgba(color.r, color.g, color.b, clamp(alpha));
}

// Scales rgb by amount (1 = unchanged, >1 brighter), clamped to [0,1]; alpha unchanged.
constexpr Color brighten(const Color& color, float amount) {
  auto clamp = [](float v) constexpr { return v < 0.0F ? 0.0F : (v > 1.0F ? 1.0F : v); };
  return Color{
      .r = clamp(color.r * amount),
      .g = clamp(color.g * amount),
      .b = clamp(color.b * amount),
      .a = color.a,
  };
}

// Linear blend from a to b; t in [0,1].
constexpr Color lerpColor(const Color& a, const Color& b, float t) {
  return Color{
      .r = a.r + (b.r - a.r) * t,
      .g = a.g + (b.g - a.g) * t,
      .b = a.b + (b.b - a.b) * t,
      .a = a.a + (b.a - a.a) * t,
  };
}

// Hue in turns ([0,1), wrapped); saturation/value/alpha in [0,1].
[[nodiscard]] Color hsv(float h, float s, float v, float a = 1.0F);
// Hue in degrees (wrapped to [0,360)); saturation/lightness/alpha in [0,1].
[[nodiscard]] Color hsl(float h, float s, float l, float a = 1.0F);
// Decomposes rgb into hue (turns, [0,1)), saturation, and value (each [0,1]).
void rgbToHsv(const Color& rgb, float& h, float& s, float& v);
// Blends from a to b through HSV space on the shortest hue path; t in [0,1].
[[nodiscard]] Color lerpHsv(const Color& a, const Color& b, float t);
// WCAG relative luminance of color, in [0,1].
[[nodiscard]] float relativeLuminance(const Color& color);
// Returns opaque black or white, whichever reads better on background.
[[nodiscard]] Color readableTextColorForBackground(const Color& background);
// Formats as "#RRGGBB" (alpha dropped).
[[nodiscard]] std::string formatRgbHex(const Color& color);
// Like hex() but non-throwing, trims whitespace, and tolerates a missing '#'.
[[nodiscard]] bool tryParseHexColor(std::string_view input, Color& out);
// Parses a CSS-style color string into out, returning false if it is not a color.
// Accepts "#rgb[a]"/"#rrggbb[aa]" (literal '#' required), plus rgb()/rgba()/hsl()/hsla()
// in either comma-separated (legacy) or space-separated (CSS Color 4, with '/' alpha) form.
// Channels take number or percentage; hue takes an optional deg/grad/rad/turn unit.
[[nodiscard]] bool tryParseCssColor(std::string_view text, Color& out);
