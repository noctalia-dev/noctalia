#pragma once

#include <cstdint>
#include <string>
#include <tuple>

namespace noctalia::theme {

  // Minimal RGB colour (0-255 per channel) with hex/HSL/ARGB conversions. The
  // custom schemes operate in HSL space via this type; the M3 schemes go
  // through HCT instead and only use this for final ARGB → hex emission.
  struct Color {
    int r = 0;
    int g = 0;
    int b = 0;

    Color() = default;
    constexpr Color(int red, int green, int blue) : r(red), g(green), b(blue) {}

    static Color fromHex(std::string_view hex); // accepts #RRGGBB or RRGGBB
    static Color fromHsl(double h, double s, double l);
    static Color fromArgb(uint32_t argb);

    std::string toHex() const;                        // "#rrggbb"
    std::tuple<double, double, double> toHsl() const; // (h°, s, l) — h in [0,360)
    uint32_t toArgb() const {
      return 0xff000000U
          | (static_cast<uint32_t>(r) << 16)
          | (static_cast<uint32_t>(g) << 8)
          | static_cast<uint32_t>(b);
    }
  };

  // Shortest circular hue distance in degrees (result in [0, 180]).
  double hueDistance(double h1, double h2);

  // Shift a colour's hue by `degrees`, preserving saturation and lightness.
  Color shiftHue(const Color& c, double degrees);

  // Build a surface colour from `base`: keep its hue, cap saturation at `sMax`,
  // force lightness to `lTarget`.
  Color adjustSurface(const Color& base, double sMax, double lTarget);

} // namespace noctalia::theme
