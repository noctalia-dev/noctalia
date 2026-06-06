#include "theme/color.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace noctalia::theme {

  namespace {

    int parseHexByte(std::string_view s, size_t offset) {
      auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9')
          return c - '0';
        if (c >= 'a' && c <= 'f')
          return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
          return c - 'A' + 10;
        throw std::invalid_argument("invalid hex digit");
      };
      return digit(s[offset]) * 16 + digit(s[offset + 1]);
    }

    int roundClamp255(double v) {
      long r = std::lround(v * 255.0);
      r = std::max<long>(r, 0);
      r = std::min<long>(r, 255);
      return static_cast<int>(r);
    }

  } // namespace

  Color Color::fromHex(std::string_view hex) {
    if (!hex.empty() && hex.front() == '#')
      hex.remove_prefix(1);
    if (hex.size() != 6)
      throw std::invalid_argument("hex must be 6 chars");
    return Color(parseHexByte(hex, 0), parseHexByte(hex, 2), parseHexByte(hex, 4));
  }

  Color Color::fromArgb(uint32_t argb) {
    return Color(
        static_cast<int>((argb >> 16) & 0xff), static_cast<int>((argb >> 8) & 0xff), static_cast<int>(argb & 0xff)
    );
  }

  std::string Color::toHex() const {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", r & 0xff, g & 0xff, b & 0xff);
    return std::string(buf);
  }

  std::tuple<double, double, double> Color::toHsl() const {
    const double rn = r / 255.0;
    const double gn = g / 255.0;
    const double bn = b / 255.0;
    const double maxC = std::max({rn, gn, bn});
    const double minC = std::min({rn, gn, bn});
    const double delta = maxC - minC;

    const double l = (maxC + minC) / 2.0;
    double h = 0.0;
    double s = 0.0;
    if (delta != 0.0) {
      if (l != 0.0 && l != 1.0) {
        s = delta / (1.0 - std::fabs(2.0 * l - 1.0));
      }
      if (maxC == rn) {
        // Positive-result fmod so negative ratios wrap cleanly onto [0, 6).
        double t = std::fmod((gn - bn) / delta, 6.0);
        if (t < 0.0)
          t += 6.0;
        h = 60.0 * t;
      } else if (maxC == gn) {
        h = 60.0 * ((bn - rn) / delta + 2.0);
      } else {
        h = 60.0 * ((rn - gn) / delta + 4.0);
      }
    }
    return {h, s, l};
  }

  Color Color::fromHsl(double h, double s, double l) {
    if (s == 0.0) {
      int v = roundClamp255(l);
      return Color(v, v, v);
    }
    const double q = (l < 0.5) ? (l * (1.0 + s)) : (l + s - l * s);
    const double p = 2.0 * l - q;
    const double hn = h / 360.0;

    auto hueToRgb = [&](double t) -> double {
      if (t < 0.0)
        t += 1.0;
      if (t > 1.0)
        t -= 1.0;
      if (t < 1.0 / 6.0)
        return p + (q - p) * 6.0 * t;
      if (t < 1.0 / 2.0)
        return q;
      if (t < 2.0 / 3.0)
        return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
      return p;
    };

    return Color(
        roundClamp255(hueToRgb(hn + 1.0 / 3.0)), roundClamp255(hueToRgb(hn)), roundClamp255(hueToRgb(hn - 1.0 / 3.0))
    );
  }

  double hueDistance(double h1, double h2) {
    const double diff = std::fabs(h1 - h2);
    return std::min(diff, 360.0 - diff);
  }

  Color shiftHue(const Color& c, double degrees) {
    auto [h, s, l] = c.toHsl();
    double newH = std::fmod(h + degrees, 360.0);
    if (newH < 0.0)
      newH += 360.0;
    return Color::fromHsl(newH, s, l);
  }

  Color adjustSurface(const Color& base, double sMax, double lTarget) {
    auto [h, s, _l] = base.toHsl();
    return Color::fromHsl(h, std::min(s, sMax), lTarget);
  }

} // namespace noctalia::theme
