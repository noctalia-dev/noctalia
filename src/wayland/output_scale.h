#pragma once

#include <cmath>
#include <cstdint>

namespace wayland {

  // Canonical fixed-point base for a per-output scale numerator (120 == 1.0),
  // matching wp_fractional_scale_v1's /120 convention.
  inline constexpr std::int32_t kScaleNumeratorBase = 120;

  // A /120 numerator is usable only when strictly positive.
  [[nodiscard]] inline bool isValidScaleNumerator(std::int32_t numerator) noexcept { return numerator > 0; }

  // Convert a floating scale factor to a /120 numerator, or 0 when the input is
  // not a usable positive finite factor.
  [[nodiscard]] inline std::int32_t scaleNumeratorFromFactor(double factor) noexcept {
    if (!std::isfinite(factor) || factor <= 0.0) {
      return 0;
    }
    const auto numerator = static_cast<std::int32_t>(std::lround(factor * static_cast<double>(kScaleNumeratorBase)));
    return numerator > 0 ? numerator : 0;
  }

  // Relative tolerance for treating a mode/logical ratio as isotropic. A real
  // output's per-axis scale agrees to well within this; larger disagreement means
  // the reported geometry is unreliable and the ratio is rejected.
  inline constexpr double kScaleAxisTolerance = 0.02;

  struct DetectedScale {
    double scale = 0.0;
    bool rotated = false;
    bool available = false;
  };

  // Resolve an output scale from its current-mode (physical) vs logical size,
  // picking the transform (normal or 90deg-rotated) whose axes agree best.
  // Rejects non-positive dimensions and anisotropic ratios (per-axis scale
  // disagreement beyond kScaleAxisTolerance).
  [[nodiscard]] inline DetectedScale detectScaleFromDimensions(
      std::int32_t physW, std::int32_t physH, std::int32_t logicalW, std::int32_t logicalH
  ) noexcept {
    if (physW <= 0 || physH <= 0 || logicalW <= 0 || logicalH <= 0) {
      return {};
    }
    const auto pw = static_cast<double>(physW);
    const auto ph = static_cast<double>(physH);
    const auto lw = static_cast<double>(logicalW);
    const auto lh = static_cast<double>(logicalH);

    const double normalX = pw / lw;
    const double normalY = ph / lh;
    const double rotatedX = pw / lh;
    const double rotatedY = ph / lw;
    const double normalDelta = std::abs(normalX - normalY);
    const double rotatedDelta = std::abs(rotatedX - rotatedY);

    const bool rotated = rotatedDelta < normalDelta;
    const double selX = rotated ? rotatedX : normalX;
    const double selY = rotated ? rotatedY : normalY;
    const double delta = rotated ? rotatedDelta : normalDelta;
    const double scale = (selX + selY) * 0.5;
    if (scale <= 0.0 || delta > kScaleAxisTolerance * scale) {
      return {};
    }
    return {.scale = scale, .rotated = rotated, .available = true};
  }

  // Resolve the canonical /120 scale numerator for one output in capability order:
  //  1. wlr-output-management fixed scale factor (headScaleFactor),
  //  2. transform-aware current-mode/logical-size ratio (detectedScaleFactor),
  //  3. integer wl_output.scale.
  // Each candidate is used only if it validates as a positive numerator;
  // otherwise the next capability is consulted. There is always exactly one
  // published numerator per output.
  [[nodiscard]] inline std::int32_t resolveConfiguredScaleNumerator(
      double headScaleFactor, double detectedScaleFactor, std::int32_t wlOutputScale
  ) noexcept {
    if (const std::int32_t numerator = scaleNumeratorFromFactor(headScaleFactor); isValidScaleNumerator(numerator)) {
      return numerator;
    }
    if (const std::int32_t numerator = scaleNumeratorFromFactor(detectedScaleFactor);
        isValidScaleNumerator(numerator)) {
      return numerator;
    }
    const std::int32_t integerScale = wlOutputScale > 0 ? wlOutputScale : 1;
    return integerScale * kScaleNumeratorBase;
  }

} // namespace wayland
