#include "ui/app_icon_colorization.h"

#include "config/config_types.h"
#include "render/core/color.h"
#include "ui/palette.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

  struct AppIconBakeTuning {
    float minDisplayLevel = 0.28f;
    float minContrastSpan = 0.12f;
  };

  [[nodiscard]] AppIconBakeTuning bakeTuningForTheme() noexcept {
    if (isResolvedLightTheme()) {
      return AppIconBakeTuning{.minDisplayLevel = 0.32f, .minContrastSpan = 0.12f};
    }
    return AppIconBakeTuning{.minDisplayLevel = 0.28f, .minContrastSpan = 0.12f};
  }

  [[nodiscard]] ColorSpec defaultAppIconColorizationTint() noexcept {
    return colorSpecFromRole(isResolvedLightTheme() ? ColorRole::OnSurface : ColorRole::OnSurfaceVariant);
  }

  [[nodiscard]] float pixelLuminance(float red, float green, float blue) noexcept {
    return 0.299f * red + 0.587f * green + 0.114f * blue;
  }

  [[nodiscard]] std::uint8_t toGrayByte(float level) noexcept {
    return static_cast<std::uint8_t>(std::lround(std::clamp(level, 0.0f, 1.0f) * 255.0f));
  }

} // namespace

ShellAppIconColorizationSettings shellAppIconColorizationSettings(const ShellConfig& shell) noexcept {
  return ShellAppIconColorizationSettings{.enabled = shell.appIconColorize, .color = shell.appIconColor};
}

Signal<>& shellAppIconColorizationChanged() noexcept {
  static Signal<> signal;
  return signal;
}

void notifyShellAppIconColorizationChanged() noexcept { shellAppIconColorizationChanged().emit(); }

std::optional<ColorSpec> effectiveShellAppIconColorizationTint(const ShellConfig& shell) noexcept {
  if (!shell.appIconColorize) {
    return std::nullopt;
  }
  if (shell.appIconColor.has_value()) {
    return shell.appIconColor;
  }
  return defaultAppIconColorizationTint();
}

AppIconColorizationStyle resolveAppIconColorization(const ColorSpec& tint) noexcept {
  return AppIconColorizationStyle{.tint = resolveColorSpec(tint), .monochrome = false};
}

void bakeAppIconForColorization(std::uint8_t* rgba, int width, int height, const Color& tint) noexcept {
  if (rgba == nullptr || width <= 0 || height <= 0) {
    return;
  }

  constexpr float kAlphaCutoff = 8.0f / 255.0f;
  const AppIconBakeTuning tuning = bakeTuningForTheme();
  const float tintLum = relativeLuminance(tint);

  float minLum = 1.0f;
  float maxLum = 0.0f;
  float inkSum = 0.0f;
  float inkWeight = 0.0f;
  bool found = false;

  const auto pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  for (std::size_t i = 0; i < pixelCount; ++i) {
    const std::size_t offset = i * 4U;
    const float alpha = static_cast<float>(rgba[offset + 3U]) / 255.0f;
    if (alpha < kAlphaCutoff) {
      continue;
    }

    const float red = static_cast<float>(rgba[offset]) / 255.0f;
    const float green = static_cast<float>(rgba[offset + 1U]) / 255.0f;
    const float blue = static_cast<float>(rgba[offset + 2U]) / 255.0f;
    const float lum = pixelLuminance(red, green, blue);
    minLum = std::min(minLum, lum);
    maxLum = std::max(maxLum, lum);
    inkSum += lum * alpha;
    inkWeight += alpha;
    found = true;
  }

  if (!found) {
    return;
  }

  const float meanInk = inkWeight > 0.0f ? inkSum / inkWeight : 0.5f;
  const bool invertInk = (tintLum < 0.45f && meanInk > 0.55f) || (tintLum > 0.55f && meanInk < 0.45f);

  const float rawSpan = maxLum - minLum;
  const bool stretch = rawSpan >= tuning.minContrastSpan;
  const float span = std::max(rawSpan, 1e-3f);
  const float pad = span * 0.04f;
  const float minMapped = std::max(0.0f, minLum - pad);
  const float mappedSpan = std::max(maxLum + pad - minMapped, 1e-3f);

  for (std::size_t i = 0; i < pixelCount; ++i) {
    const std::size_t offset = i * 4U;
    const float alpha = static_cast<float>(rgba[offset + 3U]) / 255.0f;
    if (alpha < kAlphaCutoff) {
      continue;
    }

    const float red = static_cast<float>(rgba[offset]) / 255.0f;
    const float green = static_cast<float>(rgba[offset + 1U]) / 255.0f;
    const float blue = static_cast<float>(rgba[offset + 2U]) / 255.0f;
    const float lum = pixelLuminance(red, green, blue);

    float level = stretch ? std::clamp((lum - minMapped) / mappedSpan, 0.0f, 1.0f) : lum;
    if (invertInk) {
      level = 1.0f - level;
    }
    const float displayLevel = tuning.minDisplayLevel + (1.0f - tuning.minDisplayLevel) * level;
    rgba[offset] = toGrayByte(tint.r * displayLevel);
    rgba[offset + 1U] = toGrayByte(tint.g * displayLevel);
    rgba[offset + 2U] = toGrayByte(tint.b * displayLevel);
  }
}
