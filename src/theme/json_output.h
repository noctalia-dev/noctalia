#pragma once

#include <string>

namespace noctalia::theme {

  struct GeneratedPalette;
  enum class Scheme;
  enum class Variant { Dark, Light, Both };

  // Serialize a GeneratedPalette to JSON. Values are "#rrggbb" strings emitted
  // in the canonical iteration order from tokens.h. `Both` wraps dark+light in
  // `{"dark": {...}, "light": {...}}`; `Dark`/`Light` emit a flat token map.
  std::string toJson(const GeneratedPalette& palette, Scheme scheme, Variant variant);

} // namespace noctalia::theme
