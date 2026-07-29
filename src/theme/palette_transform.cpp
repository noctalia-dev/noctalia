#include "theme/palette_transform.h"

#include "cpp/cam/hct.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace noctalia::theme {

  namespace {

    namespace mcu = material_color_utilities;

    // The dark elevation ladder, plus the two terminal tokens that are the background
    // itself. terminal_selection_bg and terminal_normal_black are deliberately absent:
    // a terminal needs them to stay visible against the background, not to follow it.
    constexpr std::array<std::string_view, 12> kSurfaceRamp = {{
        "background",
        "surface",
        "surface_variant",
        "surface_dim",
        "surface_bright",
        "surface_container_lowest",
        "surface_container_low",
        "surface_container",
        "surface_container_high",
        "surface_container_highest",
        "terminal_background",
        "terminal_cursor_text",
    }};

    [[nodiscard]] std::uint32_t lowerTone(std::uint32_t argb, double shift) {
      mcu::Hct hct(argb);
      hct.set_tone(std::max(0.0, hct.get_tone() - shift));
      return hct.ToInt();
    }

  } // namespace

  void applyPureBlackDark(TokenMap& darkTokens) {
    const auto base = darkTokens.find("surface");
    if (base == darkTokens.end()) {
      return;
    }
    // Shifting by the base tone puts `surface` on tone 0 and drops everything above it
    // by the same amount; tokens already below the base clamp to black.
    const double shift = mcu::Hct(base->second).get_tone();
    if (shift <= 0.0) {
      return;
    }
    for (const std::string_view key : kSurfaceRamp) {
      if (const auto it = darkTokens.find(std::string(key)); it != darkTokens.end()) {
        it->second = lowerTone(it->second, shift);
      }
    }
  }

  void applyPureBlackDark(GeneratedPalette& palette) {
    if (!palette.dark.empty()) {
      applyPureBlackDark(palette.dark);
    }
  }

  void applyHighContrast(TokenMap& tokens, bool isDark) {
    for (auto& [key, value] : tokens) {
      mcu::Hct hct(value);
      double tone = hct.get_tone();

      if (key == "outline" || key == "outline_variant") {
        // Force borders to be highly visible: extremely light in dark mode, extremely dark in light mode
        tone = isDark ? std::max(tone, 80.0) : std::min(tone, 20.0);
      } else {
        // Aggressive contrast stretch for everything else:
        // Push darks darker and lights lighter to separate backgrounds from foregrounds
        if (tone < 50.0) {
          tone = std::max(0.0, tone - 20.0);
        } else {
          tone = std::min(100.0, tone + 20.0);
        }
      }

      hct.set_tone(tone);
      value = hct.ToInt();
    }
  }

  void applyHighContrast(GeneratedPalette& palette) {
    if (!palette.dark.empty()) {
      applyHighContrast(palette.dark, true);
    }
    if (!palette.light.empty()) {
      applyHighContrast(palette.light, false);
    }
  }

} // namespace noctalia::theme
