#include "theme/color.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
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

    // sRGB gamma decode/encode (IEC 61966-2-1)
    double linearize(double c) {
      return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    }

    double delinearize(double c) {
      return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
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

  std::tuple<double, double, double> Color::toOklch() const {
    // sRGB → linear RGB
    const double lr = linearize(r / 255.0);
    const double lg = linearize(g / 255.0);
    const double lb = linearize(b / 255.0);

    // linear RGB → OkLab (Björn Ottosson, 2020)
    const double lms_l = std::cbrt(0.4122214708 * lr + 0.5363325363 * lg + 0.0514459929 * lb);
    const double lms_m = std::cbrt(0.2119034982 * lr + 0.6806995451 * lg + 0.1073969566 * lb);
    const double lms_s = std::cbrt(0.0883024619 * lr + 0.2817188376 * lg + 0.6299787005 * lb);

    const double L   = 0.2104542553 * lms_l + 0.7936177850 * lms_m - 0.0040720468 * lms_s;
    const double lab_a = 1.9779984951 * lms_l - 2.4285922050 * lms_m + 0.4505937099 * lms_s;
    const double lab_b = 0.0259040371 * lms_l + 0.7827717662 * lms_m - 0.8086757660 * lms_s;

    // OkLab → OkLCH
    const double C = std::sqrt(lab_a * lab_a + lab_b * lab_b);
    double H = std::atan2(lab_b, lab_a) * (180.0 / std::numbers::pi);
    if (H < 0.0)
      H += 360.0;

    return {L, C, H};
  }

  Color Color::fromOklch(double L, double C, double h) {
    const double hRad  = h * (std::numbers::pi / 180.0);
    const double lab_a = C * std::cos(hRad);
    const double lab_b = C * std::sin(hRad);

    // OkLab → linear RGB (Björn Ottosson, 2020)
    const double lms_l = L + 0.3963377774 * lab_a + 0.2158037573 * lab_b;
    const double lms_m = L - 0.1055613458 * lab_a - 0.0638541728 * lab_b;
    const double lms_s = L - 0.0894841775 * lab_a - 1.2914855480 * lab_b;

    const double l = lms_l * lms_l * lms_l;
    const double m = lms_m * lms_m * lms_m;
    const double s = lms_s * lms_s * lms_s;

    const double lr = std::clamp( 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s, 0.0, 1.0);
    const double lg = std::clamp(-1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s, 0.0, 1.0);
    const double lb = std::clamp(-0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s, 0.0, 1.0);

    // linear RGB → sRGB
    return Color(roundClamp255(delinearize(lr)), roundClamp255(delinearize(lg)), roundClamp255(delinearize(lb)));
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
