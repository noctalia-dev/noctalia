#include "theme/contrast.h"

#include <algorithm>
#include <cmath>

namespace noctalia::theme {

  namespace {

    struct Oklch {
      double lightness;
      double chroma;
      double hueRadians;
    };

    struct LinearRgb {
      double red;
      double green;
      double blue;
    };

    double linearizeChannel(int channel) {
      const double normalized = channel / 255.0;
      if (normalized <= 0.04045)
        return normalized / 12.92;
      return std::pow((normalized + 0.055) / 1.055, 2.4);
    }

    double delinearizeChannel(double channel) {
      if (channel <= 0.0031308)
        return channel * 12.92;
      return 1.055 * std::pow(channel, 1.0 / 2.4) - 0.055;
    }

    Oklch toOklch(const Color& color) {
      const double red = linearizeChannel(color.r);
      const double green = linearizeChannel(color.g);
      const double blue = linearizeChannel(color.b);

      const double l = std::cbrt(0.4122214708 * red + 0.5363325363 * green + 0.0514459929 * blue);
      const double m = std::cbrt(0.2119034982 * red + 0.6806995451 * green + 0.1073969566 * blue);
      const double s = std::cbrt(0.0883024619 * red + 0.2817188376 * green + 0.6299787005 * blue);

      const double lightness = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
      const double a = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
      const double b = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
      return {
          .lightness = lightness,
          .chroma = std::hypot(a, b),
          .hueRadians = std::atan2(b, a),
      };
    }

    LinearRgb fromOklch(const Oklch& color) {
      const double a = color.chroma * std::cos(color.hueRadians);
      const double b = color.chroma * std::sin(color.hueRadians);
      const double lRoot = color.lightness + 0.3963377774 * a + 0.2158037573 * b;
      const double mRoot = color.lightness - 0.1055613458 * a - 0.0638541728 * b;
      const double sRoot = color.lightness - 0.0894841775 * a - 1.2914855480 * b;
      const double l = lRoot * lRoot * lRoot;
      const double m = mRoot * mRoot * mRoot;
      const double s = sRoot * sRoot * sRoot;
      return {
          .red = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
          .green = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
          .blue = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s,
      };
    }

    bool isInSrgbGamut(const LinearRgb& color) {
      constexpr double tolerance = 1e-7;
      return color.red >= -tolerance
          && color.red <= 1.0 + tolerance
          && color.green >= -tolerance
          && color.green <= 1.0 + tolerance
          && color.blue >= -tolerance
          && color.blue <= 1.0 + tolerance;
    }

    Color toSrgbColor(const LinearRgb& color) {
      auto toByte = [](double channel) {
        const double srgb = std::clamp(delinearizeChannel(channel), 0.0, 1.0);
        return static_cast<int>(std::round(srgb * 255.0));
      };
      return Color(toByte(color.red), toByte(color.green), toByte(color.blue));
    }

    Color gamutMapToSrgb(Oklch color) {
      LinearRgb linear = fromOklch(color);
      if (isInSrgbGamut(linear))
        return toSrgbColor(linear);

      double low = 0.0;
      double high = color.chroma;
      color.chroma = 0.0;
      LinearRgb best = fromOklch(color);
      for (int i = 0; i < 20; ++i) {
        const double candidateChroma = (low + high) / 2.0;
        color.chroma = candidateChroma;
        const LinearRgb candidate = fromOklch(color);
        if (isInSrgbGamut(candidate)) {
          low = candidateChroma;
          best = candidate;
        } else {
          high = candidateChroma;
        }
      }
      return toSrgbColor(best);
    }

  } // namespace

  double relativeLuminance(int r, int g, int b) {
    return 0.2126 * linearizeChannel(r) + 0.7152 * linearizeChannel(g) + 0.0722 * linearizeChannel(b);
  }

  double contrastRatio(const Color& a, const Color& b) {
    const double l1 = relativeLuminance(a.r, a.g, a.b);
    const double l2 = relativeLuminance(b.r, b.g, b.b);
    const double lighter = std::max(l1, l2);
    const double darker = std::min(l1, l2);
    return (lighter + 0.05) / (darker + 0.05);
  }

  bool isDark(const Color& c) { return relativeLuminance(c.r, c.g, c.b) < 0.179; }

  Color ensureContrast(const Color& foreground, const Color& background, double minRatio, int preferLight) {
    if (contrastRatio(foreground, background) >= minRatio)
      return foreground;

    const Oklch source = toOklch(foreground);
    bool lighten;
    if (preferLight > 0)
      lighten = true;
    else if (preferLight < 0)
      lighten = false;
    else
      lighten = isDark(background);

    const auto endpointAt = [&](double lightness) {
      return gamutMapToSrgb(
          Oklch{
              .lightness = lightness,
              .chroma = source.chroma,
              .hueRadians = source.hueRadians,
          }
      );
    };
    const Color preferredEndpoint = endpointAt(lighten ? 1.0 : 0.0);
    const double preferredEndpointRatio = contrastRatio(preferredEndpoint, background);
    if (preferredEndpointRatio < minRatio) {
      const Color oppositeEndpoint = endpointAt(lighten ? 0.0 : 1.0);
      return contrastRatio(oppositeEndpoint, background) > preferredEndpointRatio ? oppositeEndpoint
                                                                                  : preferredEndpoint;
    }

    double low = lighten ? source.lightness : 0.0;
    double high = lighten ? 1.0 : source.lightness;

    Color best = preferredEndpoint;
    for (int i = 0; i < 20; ++i) {
      const double lightness = (low + high) / 2.0;
      const Color test = gamutMapToSrgb(
          Oklch{
              .lightness = lightness,
              .chroma = source.chroma,
              .hueRadians = source.hueRadians,
          }
      );
      if (contrastRatio(test, background) >= minRatio) {
        best = test;
        if (lighten)
          high = lightness;
        else
          low = lightness;
      } else {
        if (lighten)
          low = lightness;
        else
          high = lightness;
      }
    }
    return best;
  }

} // namespace noctalia::theme
