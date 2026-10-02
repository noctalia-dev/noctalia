// synthesizeTerminalPaletteTokens() used to fill the "bright" ANSI slots with a
// byte-for-byte copy of the "normal" slots, and derived magenta/cyan from
// primary_fixed_dim/secondary_fixed_dim, which keep their base's hue by
// construction, so they collided with green/yellow whenever a scheme did not
// separate those roles by hue on its own. terminal_normal_black also bypassed
// the contrast fix entirely. See issue #4497.

#include "tests/test_check.h"
#include "theme/color.h"
#include "theme/contrast.h"
#include "theme/fixed_palette.h"
#include "theme/tokens.h"

// noctalia::theme::Color and the unrelated global ::Color (pulled in
// transitively through theme/fixed_palette.h -> ui/palette.h) share the same
// name, so this file always spells out noctalia::theme::Color; an unqualified
// `Color`, even with a using-declaration, is ambiguous between the two.

namespace {

  using noctalia::theme::TokenMap;

  TokenMap makeTokens(uint32_t primary, uint32_t primaryFixedDim, uint32_t secondary, uint32_t secondaryFixedDim) {
    TokenMap tokens;
    tokens["background"] = noctalia::theme::Color(20, 20, 24).toArgb();
    tokens["on_surface"] = noctalia::theme::Color(230, 230, 235).toArgb();
    tokens["surface"] = noctalia::theme::Color(20, 20, 24).toArgb();
    tokens["surface_variant"] = noctalia::theme::Color(70, 70, 79).toArgb();
    tokens["on_surface_variant"] = noctalia::theme::Color(200, 200, 205).toArgb();
    tokens["outline"] = noctalia::theme::Color(145, 144, 154).toArgb();
    tokens["error"] = noctalia::theme::Color(255, 180, 171).toArgb();
    tokens["primary"] = primary;
    tokens["primary_fixed_dim"] = primaryFixedDim;
    tokens["secondary"] = secondary;
    tokens["secondary_fixed_dim"] = secondaryFixedDim;
    tokens["tertiary"] = noctalia::theme::Color(231, 185, 213).toArgb();
    return tokens;
  }

} // namespace

int main() {
  // primary == primary_fixed_dim and secondary == secondary_fixed_dim
  // reproduces the exact collision from issue #4497: both share hue with
  // their fixed-dim counterpart, so a fix that only adjusts lightness would
  // make green and magenta (and yellow and cyan) come out identical.
  const uint32_t primary = noctalia::theme::Color(189, 194, 255).toArgb();
  const uint32_t secondary = noctalia::theme::Color(197, 196, 221).toArgb();
  TokenMap tokens = makeTokens(primary, primary, secondary, secondary);

  noctalia::theme::synthesizeTerminalPaletteTokens(tokens);

  const auto hue = [&](std::string_view key) {
    const auto [h, s, l] = noctalia::theme::Color::fromArgb(tokens.at(std::string(key))).toHsl();
    (void)s;
    (void)l;
    return h;
  };

  // Every "bright" slot must differ from its "normal" counterpart.
  for (std::string_view name : {"black", "red", "green", "yellow", "blue", "magenta", "cyan", "white"}) {
    const std::string normalKey = "terminal_normal_" + std::string(name);
    const std::string brightKey = "terminal_bright_" + std::string(name);
    TEST_CHECK(tokens.at(normalKey) != tokens.at(brightKey));
  }

  // Two different semantic ANSI hues must stay visually distinguishable,
  // even when their M3 source roles share a hue by construction. The
  // separation is applied in HSL space (kMinAnsiHueDistance degrees) but the
  // final value also passes through the OKLCH-based contrast fix, which can
  // drift the HSL-measured hue by a degree or two; the bound below is well
  // under that separation and far above the pre-fix collision (0 degrees).
  TEST_CHECK(noctalia::theme::hueDistance(hue("terminal_normal_green"), hue("terminal_normal_magenta")) >= 20.0);
  TEST_CHECK(noctalia::theme::hueDistance(hue("terminal_normal_yellow"), hue("terminal_normal_cyan")) >= 20.0);

  // terminal_normal_black and terminal_bright_black must stay readable
  // against the background instead of following it (see palette_transform.cpp).
  const noctalia::theme::Color background = noctalia::theme::Color::fromArgb(tokens.at("terminal_background"));
  TEST_CHECK(
      noctalia::theme::contrastRatio(noctalia::theme::Color::fromArgb(tokens.at("terminal_normal_black")), background)
      >= 4.49
  );
  TEST_CHECK(
      noctalia::theme::contrastRatio(noctalia::theme::Color::fromArgb(tokens.at("terminal_bright_black")), background)
      >= 4.49
  );

  return 0;
}
