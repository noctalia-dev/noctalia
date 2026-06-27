#include "theme/color.h"
#include "theme/palette_generator.h"
#include "theme/scheme.h"

#include <array>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

  bool fail(std::string_view message) {
    std::fprintf(stderr, "scheme_test: FAIL: %.*s\n", static_cast<int>(message.size()), message.data());
    return false;
  }

  bool expect(bool condition, std::string_view message) {
    if (!condition)
      return fail(message);
    return true;
  }

  std::vector<uint8_t> makeColorfulBuffer() {
    std::vector<uint8_t> rgb(112u * 112u * 3u);
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
        const std::size_t i = (y * 112u + x) * 3u;
        rgb[i + 0] = color[0];
        rgb[i + 1] = color[1];
        rgb[i + 2] = color[2];
      }
    }
    return rgb;
  }

  uint32_t token(const noctalia::theme::GeneratedPalette& palette, const std::string& name) {
    const auto it = palette.dark.find(name);
    return it == palette.dark.end() ? 0u : it->second;
  }

  double saturation(uint32_t argb) {
    const auto color = noctalia::theme::Color::fromArgb(argb);
    const auto [h, s, l] = color.toHsl();
    (void)h;
    (void)l;
    return s;
  }

  bool checkSchemeStrings() {
    using noctalia::theme::Scheme;
    bool ok = true;
    const auto parsed = noctalia::theme::schemeFromString("soft");
    ok = expect(parsed.has_value() && *parsed == Scheme::Soft, "parses canonical soft scheme") && ok;
    ok = expect(noctalia::theme::schemeToString(Scheme::Soft) == "soft", "serializes canonical soft scheme") && ok;
    ok = expect(!noctalia::theme::schemeFromString("softened").has_value(), "rejects non-canonical soft alias") && ok;
    return ok;
  }

  bool checkSoftGeneration() {
    using noctalia::theme::Scheme;

    const auto rgb = makeColorfulBuffer();
    const auto faithful = noctalia::theme::generateCustom(rgb, Scheme::Faithful);
    const auto soft = noctalia::theme::generateCustom(rgb, Scheme::Soft);
    const auto muted = noctalia::theme::generateCustom(rgb, Scheme::Muted);

    bool ok = true;
    const std::string required[] = {
        "source_color", "primary",   "on_primary", "surface",
        "on_surface",   "secondary", "tertiary",   "terminal_normal_red",
    };
    for (const auto& name : required) {
      ok = expect(soft.dark.contains(name), "soft dark palette has required token " + name) && ok;
      ok = expect(soft.light.contains(name), "soft light palette has required token " + name) && ok;
    }

    ok = expect(
             token(soft, "source_color") == token(faithful, "source_color"), "soft uses faithful source color selection"
         )
        && ok;

    const double mutedPrimary = saturation(token(muted, "primary"));
    const double softPrimary = saturation(token(soft, "primary"));
    const double faithfulPrimary = saturation(token(faithful, "primary"));
    ok = expect(
             mutedPrimary < softPrimary && softPrimary < faithfulPrimary,
             "soft primary saturation sits between muted and faithful"
         )
        && ok;

    const double mutedSurface = saturation(token(muted, "surface"));
    const double softSurface = saturation(token(soft, "surface"));
    const double faithfulSurface = saturation(token(faithful, "surface"));
    ok = expect(
             mutedSurface < softSurface && softSurface < faithfulSurface,
             "soft surface saturation sits between muted and faithful"
         )
        && ok;

    return ok;
  }

} // namespace

int main() {
  bool ok = true;
  ok = checkSchemeStrings() && ok;
  ok = checkSoftGeneration() && ok;
  return ok ? 0 : 1;
}
