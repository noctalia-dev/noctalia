#include "theme/fixed_palette.h"

#include "cpp/cam/cam.h"
#include "cpp/cam/hct.h"
#include "cpp/utils/utils.h"
#include "render/core/color.h"
#include "theme/color.h"
#include "theme/contrast.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <tuple>
#include <vector>

namespace noctalia::theme {

  namespace {

    constexpr std::uint32_t kOpaqueBlack = 0xff000000U;
    constexpr std::uint32_t kOpaqueWhite = 0xffffffffU;

    ::Color tokenToColor(const TokenMap& tokens, std::string_view key) {
      auto it = tokens.find(std::string(key));
      if (it == tokens.end()) {
        return hex("#ff00ff");
      }
      return rgbHex(it->second & 0x00FFFFFFU);
    }

    Color toUiColor(const ::Color& color) {
      auto toByte = [](float value) { return static_cast<int>(std::clamp(value, 0.0F, 1.0F) * 255.0F + 0.5F); };
      return Color(toByte(color.r), toByte(color.g), toByte(color.b));
    }

    void setToken(TokenMap& dst, std::string_view key, const Color& color) { dst[std::string(key)] = color.toArgb(); }

    std::uint32_t colorToArgb(const ::Color& color) {
      auto toByte = [](float value) {
        return static_cast<std::uint32_t>(value <= 0.0F ? 0.0F : (value >= 1.0F ? 255.0F : value * 255.0F + 0.5F));
      };
      return kOpaqueBlack | (toByte(color.r) << 16U) | (toByte(color.g) << 8U) | toByte(color.b);
    }

    void setToken(TokenMap& dst, std::string_view key, const ::Color& color) {
      dst[std::string(key)] = colorToArgb(color);
    }

    bool hasToken(const TokenMap& tokens, std::string_view key) { return tokens.contains(std::string(key)); }

    std::uint32_t tokenOr(const TokenMap& tokens, std::string_view key, std::uint32_t fallback) {
      auto it = tokens.find(std::string(key));
      return it == tokens.end() ? fallback : it->second;
    }

    void setMissingToken(TokenMap& tokens, std::string_view key, std::uint32_t value) {
      if (!hasToken(tokens, key)) {
        tokens[std::string(key)] = value;
      }
    }

    const ::Color& ansiColorForKey(const TerminalAnsiColors& colors, std::string_view key) {
      if (key == kTerminalBlackJsonKey) {
        return colors.black;
      }
      if (key == kTerminalRedJsonKey) {
        return colors.red;
      }
      if (key == kTerminalGreenJsonKey) {
        return colors.green;
      }
      if (key == kTerminalYellowJsonKey) {
        return colors.yellow;
      }
      if (key == kTerminalBlueJsonKey) {
        return colors.blue;
      }
      if (key == kTerminalMagentaJsonKey) {
        return colors.magenta;
      }
      if (key == kTerminalCyanJsonKey) {
        return colors.cyan;
      }
      return colors.white;
    }

    const ::Color& directColorForKey(const TerminalPalette& terminal, std::string_view key) {
      if (key == kTerminalForegroundJsonKey) {
        return terminal.foreground;
      }
      if (key == kTerminalBackgroundJsonKey) {
        return terminal.background;
      }
      if (key == kTerminalCursorJsonKey) {
        return terminal.cursor;
      }
      if (key == kTerminalCursorTextJsonKey) {
        return terminal.cursorText;
      }
      if (key == kTerminalSelectionFgJsonKey) {
        return terminal.selectionFg;
      }
      return terminal.selectionBg;
    }

    const TerminalAnsiColors& ansiGroupForKey(const TerminalPalette& terminal, std::string_view key) {
      return key == kTerminalBrightJsonKey ? terminal.bright : terminal.normal;
    }

    ::Color& directColorSlot(TerminalPalette& terminal, std::string_view key) {
      if (key == kTerminalForegroundJsonKey) {
        return terminal.foreground;
      }
      if (key == kTerminalBackgroundJsonKey) {
        return terminal.background;
      }
      if (key == kTerminalCursorJsonKey) {
        return terminal.cursor;
      }
      if (key == kTerminalCursorTextJsonKey) {
        return terminal.cursorText;
      }
      if (key == kTerminalSelectionFgJsonKey) {
        return terminal.selectionFg;
      }
      return terminal.selectionBg;
    }

    void applyAnsiColors(TokenMap& tokens, std::string_view prefix, const TerminalAnsiColors& colors) {
      for (const auto key : kTerminalAnsiColorJsonKeys) {
        std::string tokenKey(prefix);
        tokenKey += '_';
        tokenKey += key;
        setToken(tokens, tokenKey, ansiColorForKey(colors, key));
      }
    }

    void setAnsiColor(TerminalAnsiColors& colors, std::string_view jsonKey, const ::Color& color) {
      if (jsonKey == kTerminalBlackJsonKey) {
        colors.black = color;
      } else if (jsonKey == kTerminalRedJsonKey) {
        colors.red = color;
      } else if (jsonKey == kTerminalGreenJsonKey) {
        colors.green = color;
      } else if (jsonKey == kTerminalYellowJsonKey) {
        colors.yellow = color;
      } else if (jsonKey == kTerminalBlueJsonKey) {
        colors.blue = color;
      } else if (jsonKey == kTerminalMagentaJsonKey) {
        colors.magenta = color;
      } else if (jsonKey == kTerminalCyanJsonKey) {
        colors.cyan = color;
      } else {
        colors.white = color;
      }
    }

    Color interpolateColor(const Color& a, const Color& b, double t) {
      auto mix = [t](int lhs, int rhs) { return static_cast<int>(lhs + (rhs - lhs) * t); };
      return Color(mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b));
    }

    std::uint32_t ensureTerminalTextContrast(std::uint32_t foreground, std::uint32_t background) {
      return ensureContrast(Color::fromArgb(foreground), Color::fromArgb(background), 4.5).toArgb();
    }

    // Lightness step between a normal ANSI color and its bright variant: lighter on dark
    // backgrounds, darker on light ones.
    constexpr double kAnsiBrightToneStep = 12.0;
    // Below this chroma a hue is noise (monochrome schemes): it neither harmonizes nor separates.
    constexpr double kAnsiMinHueChroma = 12.0;

    struct AnsiPair {
      std::uint32_t normal;
      std::uint32_t bright;
    };

    struct AnsiColors {
      AnsiPair red;
      AnsiPair green;
      AnsiPair yellow;
      AnsiPair blue;
      AnsiPair magenta;
      AnsiPair cyan;
    };

    double brightTone(double tone, bool darkBackground) {
      return std::clamp(darkBackground ? tone + kAnsiBrightToneStep : tone - kAnsiBrightToneStep, 0.0, 100.0);
    }

    // --- Palette style: ANSI colors come from the scheme roles, so they look like the wallpaper.

    // Two ANSI colors closer than this (CAM16-UCS distance) would read as the same color.
    constexpr double kAnsiMinDistance = 8.0;
    constexpr double kAnsiSeparationHueStep = 24.0;
    constexpr double kAnsiSeparationToneStep = 8.0;
    // Scheme roles already sit at light tones in dark mode, so a full lightness step would wash
    // bright colors out to white: palette brights step less and gain saturation instead.
    constexpr double kPaletteBrightToneStep = 6.0;
    constexpr double kPaletteBrightChromaScale = 1.35;
    // Minimum CAM16-UCS distance between a palette bright color and its own normal color.
    constexpr double kAnsiMinBrightDistance = 3.0;
    constexpr int kAnsiMaxSeparationSteps = 6;
    constexpr double kAnsiBrightSeparationToneStep = 2.0;

    double ucsDistance(const material_color_utilities::Hct& a, const material_color_utilities::Hct& b) {
      const material_color_utilities::Cam camA = material_color_utilities::CamFromInt(a.ToInt());
      const material_color_utilities::Cam camB = material_color_utilities::CamFromInt(b.ToInt());
      return std::hypot(camA.jstar - camB.jstar, camA.astar - camB.astar, camA.bstar - camB.bstar);
    }

    double nearestDistance(
        const material_color_utilities::Hct& color, const std::vector<material_color_utilities::Hct>& taken
    ) {
      double nearest = std::numeric_limits<double>::max();
      for (const auto& other : taken) {
        nearest = std::min(nearest, ucsDistance(color, other));
      }
      return nearest;
    }

    // Lightness range a candidate color must stay inside.
    struct ToneRange {
      double min = 0.0;
      double max = 100.0;

      [[nodiscard]] bool contains(double tone) const { return tone >= min && tone <= max; }
    };

    // Returns the readable variant of `color` closest to it that does not clash with `taken`:
    // hue steps first when it has a hue, then lightness steps, each alternating directions and
    // staying inside `range`. When every candidate clashes, keeps the one farthest from its
    // nearest neighbor.
    material_color_utilities::Hct distinctColor(
        const material_color_utilities::Hct& color, const std::vector<material_color_utilities::Hct>& taken,
        std::uint32_t background, ToneRange range, double toneStep
    ) {
      const auto readable = [&](double hue, double tone) {
        return material_color_utilities::Hct(ensureTerminalTextContrast(
            material_color_utilities::Hct(hue, color.get_chroma(), std::clamp(tone, 0.0, 100.0)).ToInt(), background
        ));
      };
      material_color_utilities::Hct best = readable(color.get_hue(), color.get_tone());
      double bestDistance = nearestDistance(best, taken);
      const auto consider = [&](double hue, double tone) {
        if (bestDistance >= kAnsiMinDistance || !range.contains(tone)) {
          return;
        }
        const material_color_utilities::Hct candidate = readable(hue, tone);
        if (!range.contains(candidate.get_tone())) {
          return;
        }
        const double distance = nearestDistance(candidate, taken);
        if (distance > bestDistance) {
          best = candidate;
          bestDistance = distance;
        }
      };
      if (color.get_chroma() >= kAnsiMinHueChroma) {
        for (int step = 1; step <= kAnsiMaxSeparationSteps; ++step) {
          for (const double sign : {1.0, -1.0}) {
            consider(
                material_color_utilities::SanitizeDegreesDouble(color.get_hue() + sign * step * kAnsiSeparationHueStep),
                color.get_tone()
            );
          }
        }
      }
      for (int step = 1; step <= kAnsiMaxSeparationSteps; ++step) {
        for (const double sign : {1.0, -1.0}) {
          consider(color.get_hue(), color.get_tone() + sign * step * toneStep);
        }
      }
      return best;
    }

    AnsiColors paletteAnsiColors(const TokenMap& tokens, std::uint32_t foreground, std::uint32_t background) {
      const std::uint32_t primary = tokenOr(tokens, "primary", foreground);
      const std::uint32_t secondary = tokenOr(tokens, "secondary", primary);
      const std::array<std::uint32_t, 6> roles = {
          tokenOr(tokens, "error", foreground),
          primary,
          secondary,
          tokenOr(tokens, "tertiary", secondary),
          tokenOr(tokens, "primary_fixed_dim", primary),
          tokenOr(tokens, "secondary_fixed_dim", secondary),
      };
      const bool darkBackground = isDark(Color::fromArgb(background));
      // Normal colors stay one bright step away from white (dark) or black (light), so their
      // bright variants always have room to differ.
      const double minNormalTone = darkBackground ? 0.0 : kPaletteBrightToneStep;
      const double maxNormalTone = darkBackground ? 100.0 - kPaletteBrightToneStep : 100.0;
      std::vector<material_color_utilities::Hct> normals;
      std::vector<material_color_utilities::Hct> brights;
      normals.reserve(roles.size());
      brights.reserve(roles.size());
      std::array<AnsiPair, 6> pairs{};
      for (std::size_t i = 0; i < roles.size(); ++i) {
        material_color_utilities::Hct role(roles[i]);
        role.set_tone(std::clamp(role.get_tone(), minNormalTone, maxNormalTone));
        const material_color_utilities::Hct normal =
            distinctColor(role, normals, background, ToneRange{}, kAnsiSeparationToneStep);
        const material_color_utilities::Hct brightBase(
            normal.get_hue(), normal.get_chroma() * kPaletteBrightChromaScale,
            darkBackground ? normal.get_tone() + kPaletteBrightToneStep : normal.get_tone() - kPaletteBrightToneStep
        );
        // Bright stays on the bright side of its normal color: lighter on dark backgrounds, darker
        // on light ones. Gray schemes leave little room there, so it searches in finer steps.
        const ToneRange brightRange = darkBackground ? ToneRange{.min = normal.get_tone() + 1.0, .max = 100.0}
                                                     : ToneRange{.min = 0.0, .max = normal.get_tone() - 1.0};
        // The bright variant must differ from the other brights. Against its own normal it only
        // needs to be visibly different, so it keeps the normal's hue whenever lightness allows.
        material_color_utilities::Hct bright =
            distinctColor(brightBase, brights, background, brightRange, kAnsiBrightSeparationToneStep);
        if (ucsDistance(bright, normal) < kAnsiMinBrightDistance) {
          brights.push_back(normal);
          bright = distinctColor(brightBase, brights, background, brightRange, kAnsiBrightSeparationToneStep);
          brights.pop_back();
        }
        brights.push_back(bright);
        pairs[i] = AnsiPair{.normal = normal.ToInt(), .bright = bright.ToInt()};
        normals.push_back(normal);
      }
      return AnsiColors{
          .red = pairs[0],
          .green = pairs[1],
          .yellow = pairs[2],
          .blue = pairs[3],
          .magenta = pairs[4],
          .cyan = pairs[5],
      };
    }

    // --- Standard style: canonical ANSI hues keep their meaning (diff red/green, warnings yellow).

    // HCT hues of the sRGB primaries and secondaries.
    constexpr double kAnsiRedHue = 27.0;
    constexpr double kAnsiYellowHue = 100.0;
    constexpr double kAnsiGreenHue = 142.0;
    constexpr double kAnsiCyanHue = 196.0;
    constexpr double kAnsiBlueHue = 270.0;
    constexpr double kAnsiMagentaHue = 334.0;

    constexpr double kAnsiMinChroma = 36.0;
    constexpr double kAnsiMaxChroma = 64.0;
    constexpr double kAnsiMaxHarmonizeDegrees = 15.0;
    constexpr double kAnsiDarkNormalTone = 70.0;
    constexpr double kAnsiLightNormalTone = 45.0;

    // Material's custom-color harmonization: rotate toward the accent by half the hue distance, at most 15 degrees.
    double harmonizeHue(double hue, const material_color_utilities::Hct& accent) {
      if (accent.get_chroma() < kAnsiMinHueChroma) {
        return hue;
      }
      const double rotation =
          std::min(material_color_utilities::DiffDegrees(hue, accent.get_hue()) * 0.5, kAnsiMaxHarmonizeDegrees);
      return material_color_utilities::SanitizeDegreesDouble(
          hue + rotation * material_color_utilities::RotationDirection(hue, accent.get_hue())
      );
    }

    AnsiPair standardAnsiPair(double hue, const material_color_utilities::Hct& accent, std::uint32_t background) {
      const double chroma = std::clamp(accent.get_chroma(), kAnsiMinChroma, kAnsiMaxChroma);
      const double harmonizedHue = harmonizeHue(hue, accent);
      const bool darkBackground = isDark(Color::fromArgb(background));
      const double normalTone = darkBackground ? kAnsiDarkNormalTone : kAnsiLightNormalTone;
      const auto at = [&](double tone) {
        return ensureTerminalTextContrast(
            material_color_utilities::Hct(harmonizedHue, chroma, tone).ToInt(), background
        );
      };
      return AnsiPair{.normal = at(normalTone), .bright = at(brightTone(normalTone, darkBackground))};
    }

    AnsiColors standardAnsiColors(const TokenMap& tokens, std::uint32_t foreground, std::uint32_t background) {
      const material_color_utilities::Hct accent(tokenOr(tokens, "primary", foreground));
      return AnsiColors{
          .red = standardAnsiPair(kAnsiRedHue, accent, background),
          .green = standardAnsiPair(kAnsiGreenHue, accent, background),
          .yellow = standardAnsiPair(kAnsiYellowHue, accent, background),
          .blue = standardAnsiPair(kAnsiBlueHue, accent, background),
          .magenta = standardAnsiPair(kAnsiMagentaHue, accent, background),
          .cyan = standardAnsiPair(kAnsiCyanHue, accent, background),
      };
    }

    void writeAnsiColors(TokenMap& tokens, const AnsiColors& colors, bool onlyMissing) {
      const auto put = [&](std::string_view key, std::uint32_t value) {
        if (onlyMissing) {
          setMissingToken(tokens, key, value);
        } else {
          tokens[std::string(key)] = value;
        }
      };
      put("terminal_normal_red", colors.red.normal);
      put("terminal_normal_green", colors.green.normal);
      put("terminal_normal_yellow", colors.yellow.normal);
      put("terminal_normal_blue", colors.blue.normal);
      put("terminal_normal_magenta", colors.magenta.normal);
      put("terminal_normal_cyan", colors.cyan.normal);
      put("terminal_bright_red", colors.red.bright);
      put("terminal_bright_green", colors.green.bright);
      put("terminal_bright_yellow", colors.yellow.bright);
      put("terminal_bright_blue", colors.blue.bright);
      put("terminal_bright_magenta", colors.magenta.bright);
      put("terminal_bright_cyan", colors.cyan.bright);
    }

  } // namespace

  void applyTerminalPalette(TokenMap& tokens, const TerminalPalette& terminal) {
    for (const auto& group : kTerminalAnsiGroupTokenKeys) {
      applyAnsiColors(tokens, group.tokenPrefix, ansiGroupForKey(terminal, group.jsonKey));
    }
    for (const auto& key : kTerminalDirectColorTokenKeys) {
      setToken(tokens, key.tokenKey, directColorForKey(terminal, key.jsonKey));
    }
  }

  void synthesizeTerminalPaletteTokens(TokenMap& tokens) {
    const std::uint32_t background = tokenOr(tokens, "background", tokenOr(tokens, "surface", kOpaqueBlack));
    const std::uint32_t foreground = tokenOr(tokens, "on_surface", kOpaqueWhite);
    const std::uint32_t surface = tokenOr(tokens, "surface", background);
    const std::uint32_t surfaceVariant = tokenOr(tokens, "surface_variant", surface);
    const std::uint32_t onSurfaceVariant = tokenOr(tokens, "on_surface_variant", foreground);
    const std::uint32_t outline = tokenOr(tokens, "outline", onSurfaceVariant);

    setMissingToken(tokens, "terminal_foreground", foreground);
    setMissingToken(tokens, "terminal_background", background);
    setMissingToken(tokens, "terminal_cursor", foreground);
    setMissingToken(tokens, "terminal_cursor_text", background);
    setMissingToken(tokens, "terminal_selection_fg", onSurfaceVariant);
    setMissingToken(tokens, "terminal_selection_bg", surfaceVariant);

    setMissingToken(tokens, "terminal_normal_black", surfaceVariant);
    setMissingToken(tokens, "terminal_normal_white", onSurfaceVariant);
    setMissingToken(tokens, "terminal_bright_black", outline);
    setMissingToken(tokens, "terminal_bright_white", foreground);

    const std::uint32_t terminalBackground = tokenOr(tokens, "terminal_background", background);
    writeAnsiColors(tokens, paletteAnsiColors(tokens, foreground, terminalBackground), /*onlyMissing=*/true);
  }

  void applyStandardTerminalColors(TokenMap& tokens) {
    const std::uint32_t background = tokenOr(tokens, "background", tokenOr(tokens, "surface", kOpaqueBlack));
    const std::uint32_t foreground = tokenOr(tokens, "on_surface", kOpaqueWhite);
    const std::uint32_t terminalBackground = tokenOr(tokens, "terminal_background", background);
    writeAnsiColors(tokens, standardAnsiColors(tokens, foreground, terminalBackground), /*onlyMissing=*/false);
  }

  void applyStandardTerminalColors(GeneratedPalette& palette) {
    if (!palette.dark.empty()) {
      applyStandardTerminalColors(palette.dark);
    }
    if (!palette.light.empty()) {
      applyStandardTerminalColors(palette.light);
    }
  }

  void synthesizeTerminalPaletteTokens(GeneratedPalette& palette) {
    if (!palette.dark.empty()) {
      synthesizeTerminalPaletteTokens(palette.dark);
    }
    if (!palette.light.empty()) {
      synthesizeTerminalPaletteTokens(palette.light);
    }
  }

  TokenMap expandFixedPaletteMode(const FixedPalette& palette, bool isDark) {
    const Color primary = toUiColor(palette.primary);
    const Color onPrimary = toUiColor(palette.onPrimary);
    const Color secondary = toUiColor(palette.secondary);
    const Color onSecondary = toUiColor(palette.onSecondary);
    const Color tertiary = toUiColor(palette.tertiary);
    const Color onTertiary = toUiColor(palette.onTertiary);
    const Color error = toUiColor(palette.error);
    const Color onError = toUiColor(palette.onError);
    const Color surface = toUiColor(palette.surface);
    const Color onSurface = toUiColor(palette.onSurface);
    const Color surfaceContainer = toUiColor(palette.surfaceContainer);
    const Color onSurfaceVariant = toUiColor(palette.onSurfaceVariant);
    const Color outlineVariant = toUiColor(palette.outlineVariant);
    const Color shadow = toUiColor(palette.shadow);
    const Color hover = toUiColor(palette.hover);
    const Color onHover = toUiColor(palette.onHover);

    // A container is the accent mixed into the palette's own surface, so it keeps the accent's
    // identity (a pastel stays pastel) at a lower emphasis. Its content color is the accent itself,
    // adjusted only when it lacks contrast.
    auto makeContainer = [&surface](const Color& accent) { return interpolateColor(surface, accent, 0.30); };
    auto makeFixedDark = [](const Color& base) {
      auto [h, s, _] = base.toHsl();
      return std::make_tuple(Color::fromHsl(h, std::max(s, 0.70), 0.85), Color::fromHsl(h, std::max(s, 0.65), 0.75));
    };
    auto makeFixedLight = [](const Color& base) {
      auto [h, s, _] = base.toHsl();
      return std::make_tuple(Color::fromHsl(h, std::max(s, 0.70), 0.40), Color::fromHsl(h, std::max(s, 0.65), 0.30));
    };

    const Color primaryContainer = makeContainer(primary);
    const Color secondaryContainer = makeContainer(secondary);
    const Color tertiaryContainer = makeContainer(tertiary);
    const Color errorContainer = makeContainer(error);

    const auto [primaryH, primaryS, _primaryL] = primary.toHsl();
    const auto [secondaryH, secondaryS, _secondaryL] = secondary.toHsl();
    const auto [tertiaryH, tertiaryS, _tertiaryL] = tertiary.toHsl();

    const Color onPrimaryContainer = ensureContrast(primary, primaryContainer, 4.5);
    const Color onSecondaryContainer = ensureContrast(secondary, secondaryContainer, 4.5);
    const Color onTertiaryContainer = ensureContrast(tertiary, tertiaryContainer, 4.5);
    const Color onErrorContainer = ensureContrast(error, errorContainer, 4.5);

    const auto [primaryFixed, primaryFixedDim] = isDark ? makeFixedDark(primary) : makeFixedLight(primary);
    const auto [secondaryFixed, secondaryFixedDim] = isDark ? makeFixedDark(secondary) : makeFixedLight(secondary);
    const auto [tertiaryFixed, tertiaryFixedDim] = isDark ? makeFixedDark(tertiary) : makeFixedLight(tertiary);

    const Color onPrimaryFixed = isDark ? ensureContrast(Color::fromHsl(primaryH, 0.15, 0.15), primaryFixed, 4.5)
                                        : ensureContrast(Color::fromHsl(primaryH, 0.15, 0.90), primaryFixed, 4.5);
    const Color onPrimaryFixedVariant = isDark
        ? ensureContrast(Color::fromHsl(primaryH, 0.15, 0.20), primaryFixedDim, 4.5)
        : ensureContrast(Color::fromHsl(primaryH, 0.15, 0.85), primaryFixedDim, 4.5);
    const Color onSecondaryFixed = isDark ? ensureContrast(Color::fromHsl(secondaryH, 0.15, 0.15), secondaryFixed, 4.5)
                                          : ensureContrast(Color::fromHsl(secondaryH, 0.15, 0.90), secondaryFixed, 4.5);
    const Color onSecondaryFixedVariant = isDark
        ? ensureContrast(Color::fromHsl(secondaryH, 0.15, 0.20), secondaryFixedDim, 4.5)
        : ensureContrast(Color::fromHsl(secondaryH, 0.15, 0.85), secondaryFixedDim, 4.5);
    const Color onTertiaryFixed = isDark ? ensureContrast(Color::fromHsl(tertiaryH, 0.15, 0.15), tertiaryFixed, 4.5)
                                         : ensureContrast(Color::fromHsl(tertiaryH, 0.15, 0.90), tertiaryFixed, 4.5);
    const Color onTertiaryFixedVariant = isDark
        ? ensureContrast(Color::fromHsl(tertiaryH, 0.15, 0.20), tertiaryFixedDim, 4.5)
        : ensureContrast(Color::fromHsl(tertiaryH, 0.15, 0.85), tertiaryFixedDim, 4.5);

    const auto [surfaceH, surfaceS, surfaceL] = surface.toHsl();
    const auto [surfaceContainerH, surfaceContainerS, surfaceContainerL] = surfaceContainer.toHsl();
    const Color surfaceContainerLowest = interpolateColor(surface, surfaceContainer, 0.2);
    const Color surfaceContainerLow = interpolateColor(surface, surfaceContainer, 0.5);
    const Color surfaceContainerHigh = isDark
        ? Color::fromHsl(surfaceContainerH, surfaceContainerS, std::min(surfaceContainerL + 0.04, 0.40))
        : Color::fromHsl(surfaceContainerH, surfaceContainerS, std::max(surfaceContainerL - 0.04, 0.60));
    const Color surfaceContainerHighest = isDark
        ? Color::fromHsl(surfaceContainerH, surfaceContainerS, std::min(surfaceContainerL + 0.08, 0.45))
        : Color::fromHsl(surfaceContainerH, surfaceContainerS, std::max(surfaceContainerL - 0.08, 0.55));
    const Color surfaceDim = isDark
        ? Color::fromHsl(surfaceH, surfaceS, std::max(surfaceL - 0.04, 0.02))
        : Color::fromHsl(surfaceContainerH, surfaceContainerS, std::max(surfaceContainerL - 0.12, 0.50));
    const Color surfaceBright = isDark
        ? Color::fromHsl(surfaceContainerH, surfaceContainerS, std::min(surfaceContainerL + 0.12, 0.50))
        : Color::fromHsl(surfaceH, surfaceS, std::min(surfaceL + 0.03, 0.98));

    // outline_variant is the palette's color as given; outline is its contrast-adjusted form.
    const Color outline = ensureContrast(outlineVariant, surface, 3.0);
    const Color scrim(0, 0, 0);
    const Color inverseSurface = isDark ? Color::fromHsl(surfaceH, 0.08, 0.90) : Color::fromHsl(surfaceH, 0.08, 0.15);
    const Color inverseOnSurface = isDark ? Color::fromHsl(surfaceH, 0.05, 0.15) : Color::fromHsl(surfaceH, 0.05, 0.90);
    const Color inversePrimary = isDark ? Color::fromHsl(primaryH, std::max(primaryS * 0.8, 0.5), 0.40)
                                        : Color::fromHsl(primaryH, std::max(primaryS * 0.8, 0.5), 0.70);
    const Color background = surface;
    const Color onBackground = onSurface;

    TokenMap result;
    setToken(result, "source_color", primary);
    setToken(result, "primary", primary);
    setToken(result, "on_primary", onPrimary);
    setToken(result, "primary_container", primaryContainer);
    setToken(result, "on_primary_container", onPrimaryContainer);
    setToken(result, "primary_fixed", primaryFixed);
    setToken(result, "primary_fixed_dim", primaryFixedDim);
    setToken(result, "on_primary_fixed", onPrimaryFixed);
    setToken(result, "on_primary_fixed_variant", onPrimaryFixedVariant);
    setToken(result, "secondary", secondary);
    setToken(result, "on_secondary", onSecondary);
    setToken(result, "secondary_container", secondaryContainer);
    setToken(result, "on_secondary_container", onSecondaryContainer);
    setToken(result, "secondary_fixed", secondaryFixed);
    setToken(result, "secondary_fixed_dim", secondaryFixedDim);
    setToken(result, "on_secondary_fixed", onSecondaryFixed);
    setToken(result, "on_secondary_fixed_variant", onSecondaryFixedVariant);
    setToken(result, "tertiary", tertiary);
    setToken(result, "on_tertiary", onTertiary);
    setToken(result, "tertiary_container", tertiaryContainer);
    setToken(result, "on_tertiary_container", onTertiaryContainer);
    setToken(result, "tertiary_fixed", tertiaryFixed);
    setToken(result, "tertiary_fixed_dim", tertiaryFixedDim);
    setToken(result, "on_tertiary_fixed", onTertiaryFixed);
    setToken(result, "on_tertiary_fixed_variant", onTertiaryFixedVariant);
    setToken(result, "error", error);
    setToken(result, "on_error", onError);
    setToken(result, "error_container", errorContainer);
    setToken(result, "on_error_container", onErrorContainer);
    setToken(result, "surface", surface);
    setToken(result, "on_surface", onSurface);
    // Fixed palettes have no separate Material surface_variant; the template token mirrors surface_container.
    setToken(result, "surface_variant", surfaceContainer);
    setToken(result, "on_surface_variant", onSurfaceVariant);
    setToken(result, "surface_dim", surfaceDim);
    setToken(result, "surface_bright", surfaceBright);
    setToken(result, "surface_container_lowest", surfaceContainerLowest);
    setToken(result, "surface_container_low", surfaceContainerLow);
    setToken(result, "surface_container", surfaceContainer);
    setToken(result, "surface_container_high", surfaceContainerHigh);
    setToken(result, "surface_container_highest", surfaceContainerHighest);
    setToken(result, "surface_tint", primary);
    setToken(result, "outline", outline);
    setToken(result, "outline_variant", outlineVariant);
    setToken(result, "shadow", shadow);
    setToken(result, "hover", hover);
    setToken(result, "on_hover", onHover);
    setToken(result, "scrim", scrim);
    setToken(result, "inverse_surface", inverseSurface);
    setToken(result, "inverse_on_surface", inverseOnSurface);
    setToken(result, "inverse_primary", inversePrimary);
    setToken(result, "background", background);
    setToken(result, "on_background", onBackground);
    return result;
  }

  GeneratedPalette expandFixedPalettes(const FixedPalette& dark, const FixedPalette& light) {
    return GeneratedPalette{
        .dark = expandFixedPaletteMode(dark, true),
        .light = expandFixedPaletteMode(light, false),
    };
  }

  FixedPalette fixedPaletteFromTokens(const TokenMap& t) {
    return FixedPalette{
        .primary = tokenToColor(t, "primary"),
        .onPrimary = tokenToColor(t, "on_primary"),
        .secondary = tokenToColor(t, "secondary"),
        .onSecondary = tokenToColor(t, "on_secondary"),
        .tertiary = tokenToColor(t, "tertiary"),
        .onTertiary = tokenToColor(t, "on_tertiary"),
        .error = tokenToColor(t, "error"),
        .onError = tokenToColor(t, "on_error"),
        .surface = tokenToColor(t, "surface"),
        .onSurface = tokenToColor(t, "on_surface"),
        .surfaceContainer = tokenToColor(t, "surface_container"),
        .onSurfaceVariant = tokenToColor(t, "on_surface_variant"),
        .outlineVariant = tokenToColor(t, "outline_variant"),
        .shadow = tokenToColor(t, "shadow"),
        .hover = tokenToColor(t, "hover"),
        .onHover = tokenToColor(t, "on_hover"),
    };
  }

  ::Palette mapGeneratedPaletteMode(const TokenMap& t) {
    return ::Palette{
        .primary = tokenToColor(t, "primary"),
        .onPrimary = tokenToColor(t, "on_primary"),
        .primaryContainer = tokenToColor(t, "primary_container"),
        .onPrimaryContainer = tokenToColor(t, "on_primary_container"),
        .secondary = tokenToColor(t, "secondary"),
        .onSecondary = tokenToColor(t, "on_secondary"),
        .secondaryContainer = tokenToColor(t, "secondary_container"),
        .onSecondaryContainer = tokenToColor(t, "on_secondary_container"),
        .tertiary = tokenToColor(t, "tertiary"),
        .onTertiary = tokenToColor(t, "on_tertiary"),
        .tertiaryContainer = tokenToColor(t, "tertiary_container"),
        .onTertiaryContainer = tokenToColor(t, "on_tertiary_container"),
        .error = tokenToColor(t, "error"),
        .onError = tokenToColor(t, "on_error"),
        .errorContainer = tokenToColor(t, "error_container"),
        .onErrorContainer = tokenToColor(t, "on_error_container"),
        .surface = tokenToColor(t, "surface"),
        .onSurface = tokenToColor(t, "on_surface"),
        .onSurfaceVariant = tokenToColor(t, "on_surface_variant"),
        .surfaceContainerLow = tokenToColor(t, "surface_container_low"),
        .surfaceContainer = tokenToColor(t, "surface_container"),
        .surfaceContainerHigh = tokenToColor(t, "surface_container_high"),
        .surfaceContainerHighest = tokenToColor(t, "surface_container_highest"),
        .outlineVariant = tokenToColor(t, "outline_variant"),
        .shadow = tokenToColor(t, "shadow"),
        .scrim = tokenToColor(t, "scrim"),
        .hover = tokenToColor(t, "hover"),
        .onHover = tokenToColor(t, "on_hover"),
    };
  }

  void setGeneratedHoverTokens(GeneratedPalette& palette) {
    for (TokenMap* tokens : {&palette.dark, &palette.light}) {
      (*tokens)["hover"] = tokens->at("tertiary");
      (*tokens)["on_hover"] = tokens->at("on_tertiary");
    }
  }

  TerminalPalette terminalPaletteFromTokens(const TokenMap& tokens) {
    TerminalPalette terminal{};
    for (const auto& group : kTerminalAnsiGroupTokenKeys) {
      TerminalAnsiColors& colors = group.jsonKey == kTerminalBrightJsonKey ? terminal.bright : terminal.normal;
      for (const auto key : kTerminalAnsiColorJsonKeys) {
        std::string tokenKey(group.tokenPrefix);
        tokenKey.push_back('_');
        tokenKey.append(key);
        setAnsiColor(colors, key, tokenToColor(tokens, tokenKey));
      }
    }
    for (const auto& key : kTerminalDirectColorTokenKeys) {
      directColorSlot(terminal, key.jsonKey) = tokenToColor(tokens, key.tokenKey);
    }
    return terminal;
  }

} // namespace noctalia::theme
