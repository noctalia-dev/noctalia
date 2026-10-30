#pragma once

#include "theme/builtin_palettes.h"

#include <expected>
#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace noctalia::theme {

  // Palette JSON (community, custom, and `noctalia theme --theme-json` files) states its format in a top-level
  // integer "version"; a file without one is version 1. Version 2 names every key after the color role it feeds
  // (`on_primary`, `surface_container`, terminal `selection_fg`) where version 1 used `mOnPrimary`,
  // `mSurfaceVariant`, `selectionFg`. Older versions are upgraded on read.
  inline constexpr int kPaletteFormatVersion = 2;
  inline constexpr std::string_view kPaletteVersionJsonKey = "version";

  // The document's format version, or an error when "version" is not a positive integer or is newer than
  // kPaletteFormatVersion.
  [[nodiscard]] std::expected<int, std::string> paletteJsonVersion(const nlohmann::json& root);

  // Rewrites one fixed-palette mode object (`dark`, `light`, or a root-level palette) from `fromVersion` to
  // kPaletteFormatVersion. Only call it on fixed-palette objects, never on token maps.
  void upgradeFixedPaletteModeJson(nlohmann::json& mode, int fromVersion);

  struct ParsedPaletteFile {
    FixedPaletteMode dark;
    FixedPaletteMode light;
  };

  // Loads a community or custom palette file. Missing colors read as transparent black, as before.
  [[nodiscard]] std::optional<ParsedPaletteFile> parsePaletteFile(const std::filesystem::path& path);

} // namespace noctalia::theme
