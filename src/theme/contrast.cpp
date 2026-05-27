#include "theme/contrast.h"

#include <algorithm>
#include <cmath>

namespace noctalia::theme {

  namespace {

    double linearizeChannel(int c) {
      const double n = c / 255.0;
      if (n <= 0.03928) {
        return n / 12.92;
      }
      return std::pow((n + 0.055) / 1.055, 2.4);
    }

  } // namespace

  double relativeLuminance(int r, int g, int b) {
    return (0.2126 * linearizeChannel(r)) + (0.7152 * linearizeChannel(g)) + (0.0722 * linearizeChannel(b));
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
    if (contrastRatio(foreground, background) >= minRatio) {
      return foreground;
    }

    auto [h, s, l] = foreground.toHsl();
    bool lighten;
    if (preferLight > 0) {
      lighten = true;
    } else if (preferLight < 0) {
      lighten = false;
    } else {
      lighten = isDark(background);
    }

    double low = lighten ? l : 0.0;
    double high = lighten ? 1.0 : l;

    Color best = foreground;
    for (int i = 0; i < 20; ++i) {
      const double mid = (low + high) / 2.0;
      Color test = Color::fromHsl(h, s, mid);
      if (contrastRatio(test, background) >= minRatio) {
        best = test;
        if (lighten) {
          high = mid;
        } else {
          low = mid;
        }
      } else {
        if (lighten) {
          low = mid;
        } else {
          high = mid;
        }
      }
    }
    return best;
  }

} // namespace noctalia::theme
