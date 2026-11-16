#include "tests/test_check.h"
#include "theme/builtin_palettes.h"
#include "theme/color.h"
#include "theme/contrast.h"
#include "theme/palette_generator.h"
#include "theme/scheme.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace {

  std::vector<uint8_t> makeColorfulBuffer() {
    std::vector<uint8_t> rgb(112U * 112U * 3U);
    for (std::size_t y = 0; y < 112; ++y) {
      for (std::size_t x = 0; x < 112; ++x) {
        std::array<uint8_t, 3> color;
        if (x < 84) {
          color = {222, 59, 75};
        } else if (y < 56) {
          color = {50, 124, 222};
        } else {
          color = {236, 166, 51};
        }
        const std::size_t i = (y * 112U + x) * 3U;
        rgb[i + 0] = color[0];
        rgb[i + 1] = color[1];
        rgb[i + 2] = color[2];
      }
    }
    return rgb;
  }

  uint32_t token(const noctalia::theme::GeneratedPalette& palette, const std::string& name) {
    const auto it = palette.dark.find(name);
    return it == palette.dark.end() ? 0U : it->second;
  }

  double saturation(uint32_t argb) {
    const auto color = noctalia::theme::Color::fromArgb(argb);
    const auto [h, s, l] = color.toHsl();
    (void)h;
    (void)l;
    return s;
  }

  void checkSchemeStrings() {
    using noctalia::theme::Scheme;
    const auto parsed = noctalia::theme::schemeFromString("soft");
    TEST_CHECK(parsed.has_value() && *parsed == Scheme::Soft);
    TEST_CHECK(noctalia::theme::schemeToString(Scheme::Soft) == "soft");
    TEST_CHECK(!noctalia::theme::schemeFromString("softened").has_value());
  }

  void checkSoftGeneration() {
    using noctalia::theme::Scheme;

    const auto rgb = makeColorfulBuffer();
    const auto faithful = noctalia::theme::generateCustom(rgb, Scheme::Faithful);
    const auto soft = noctalia::theme::generateCustom(rgb, Scheme::Soft);
    const auto muted = noctalia::theme::generateCustom(rgb, Scheme::Muted);

    const std::string required[] = {
        "source_color", "primary",   "on_primary", "surface",
        "on_surface",   "secondary", "tertiary",   "terminal_normal_red",
    };
    for (const auto& name : required) {
      TEST_CHECK(soft.dark.contains(name));
      TEST_CHECK(soft.light.contains(name));
    }

    // Soft uses faithful source color selection.
    TEST_CHECK(token(soft, "source_color") == token(faithful, "source_color"));

    const double mutedPrimary = saturation(token(muted, "primary"));
    const double softPrimary = saturation(token(soft, "primary"));
    const double faithfulPrimary = saturation(token(faithful, "primary"));
    TEST_CHECK(mutedPrimary < softPrimary && softPrimary < faithfulPrimary);

    const double mutedSurface = saturation(token(muted, "surface"));
    const double softSurface = saturation(token(soft, "surface"));
    const double faithfulSurface = saturation(token(faithful, "surface"));
    TEST_CHECK(mutedSurface < softSurface && softSurface < faithfulSurface);
  }

  void checkContainerSaturationHeadroom() {
    // Fixed palette containers keep the accent's identity: a container sits between the surface and
    // its accent, and its content color stays readable on it.
    for (const auto& builtin : noctalia::theme::builtinPalettes()) {
      const auto fixed = noctalia::theme::expandBuiltinPalette(builtin);
      for (const auto* tokens : {&fixed.dark, &fixed.light}) {
        for (const std::string accent : {"primary", "secondary", "tertiary", "error"}) {
          const auto read = [tokens](const std::string& name) {
            return noctalia::theme::Color::fromArgb(tokens->at(name));
          };
          const auto surface = read("surface");
          const auto accentColor = read(accent);
          const auto container = read(accent + "_container");
          const auto onContainer = read("on_" + accent + "_container");
          TEST_CHECK(
              noctalia::theme::contrastRatio(surface, container)
              < noctalia::theme::contrastRatio(surface, accentColor) + 0.01
          );
          TEST_CHECK(noctalia::theme::contrastRatio(onContainer, container) >= 4.5);
        }
      }
    }

    const auto custom = noctalia::theme::generateCustom(makeColorfulBuffer(), noctalia::theme::Scheme::Faithful);
    const double customPrimarySaturation = saturation(token(custom, "primary"));
    const double customContainerSaturation = saturation(token(custom, "primary_container"));
    // Custom palette container saturation scales by remaining headroom.
    TEST_CHECK(customContainerSaturation < customPrimarySaturation + 0.10);
  }

  void checkPerceptualContrastAdjustment() {
    const auto background = noctalia::theme::Color::fromHex("#fbf8ff");
    const auto paleLavender = noctalia::theme::Color::fromHex("#bdc2ff");
    const auto adjusted = noctalia::theme::ensureContrast(paleLavender, background, 4.5);
    TEST_CHECK(noctalia::theme::contrastRatio(adjusted, background) >= 4.5);
    // OKLCH adjustment preserves pale lavender character.
    TEST_CHECK(adjusted.toHex() == "#6b6ea6");

    const auto readable = noctalia::theme::Color::fromHex("#5158a1");
    TEST_CHECK(noctalia::theme::ensureContrast(readable, background, 4.5).toArgb() == readable.toArgb());

    const auto impossibleBackground = noctalia::theme::Color::fromHex("#777777");
    const auto impossibleForeground = noctalia::theme::Color::fromHex("#888888");
    const auto bestPossible = noctalia::theme::ensureContrast(impossibleForeground, impossibleBackground, 7.0, 1);
    // Impossible target returns the best-contrast endpoint.
    TEST_CHECK(bestPossible.toHex() == "#000000");
  }

  void checkTerminalContrast() {
    using noctalia::theme::Scheme;

    constexpr std::array schemes{
        Scheme::TonalSpot, Scheme::Content,  Scheme::FruitSalad, Scheme::Rainbow,       Scheme::Monochrome,
        Scheme::Vibrant,   Scheme::Faithful, Scheme::Soft,       Scheme::Dysfunctional, Scheme::Muted,
    };
    constexpr std::array<std::string_view, 12> colorKeys{
        "terminal_normal_red",     "terminal_normal_green", "terminal_normal_yellow",  "terminal_normal_blue",
        "terminal_normal_magenta", "terminal_normal_cyan",  "terminal_bright_red",     "terminal_bright_green",
        "terminal_bright_yellow",  "terminal_bright_blue",  "terminal_bright_magenta", "terminal_bright_cyan",
    };
    constexpr std::array<std::string_view, 4> accentKeys{"primary", "secondary", "tertiary", "error"};

    const auto rgb = makeColorfulBuffer();
    for (const Scheme scheme : schemes) {
      const auto generated = noctalia::theme::generate(rgb, scheme);
      TEST_CHECK(generated.has_value());

      const auto checkMode = [&](const auto& tokens) {
        const auto backgroundIt = tokens.find("terminal_background");
        TEST_CHECK(backgroundIt != tokens.end());

        const auto background = noctalia::theme::Color::fromArgb(backgroundIt->second);
        for (const std::string_view key : colorKeys) {
          const auto colorIt = tokens.find(std::string(key));
          TEST_CHECK(colorIt != tokens.end());
          const auto color = noctalia::theme::Color::fromArgb(colorIt->second);
          TEST_CHECK(noctalia::theme::contrastRatio(color, background) >= 4.5);
        }

        for (const std::string_view key : accentKeys) {
          const auto backgroundColorIt = tokens.find(std::string(key));
          const auto foregroundColorIt = tokens.find("on_" + std::string(key));
          TEST_CHECK(backgroundColorIt != tokens.end() && foregroundColorIt != tokens.end());
          const auto backgroundColor = noctalia::theme::Color::fromArgb(backgroundColorIt->second);
          const auto foregroundColor = noctalia::theme::Color::fromArgb(foregroundColorIt->second);
          TEST_CHECK(noctalia::theme::contrastRatio(foregroundColor, backgroundColor) >= 4.5);
        }
      };

      checkMode(generated->dark);
      checkMode(generated->light);
    }
  }

} // namespace

int main() {
  checkSchemeStrings();
  checkSoftGeneration();
  checkContainerSaturationHeadroom();
  checkPerceptualContrastAdjustment();
  checkTerminalContrast();
  return 0;
}
