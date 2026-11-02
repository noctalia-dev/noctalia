#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

// Glyph names resolve in this order:
// 1. Explicit codepoint literals such as U+F123 or 0xF123.
// 2. Hand-curated Noctalia aliases from glyph_registry.cpp.
// 3. Native Tabler icon names from assets/fonts/tabler.json.
namespace GlyphRegistry {

  struct TablerGlyphMetadata {
    char32_t codepoint = 0;
    std::string category;
  };

  bool contains(std::string_view name);
  char32_t lookup(std::string_view name);

  // Full Tabler icon catalog with structured metadata.
  const std::unordered_map<std::string, TablerGlyphMetadata>& tablerGlyphMetadata();
  // Full Tabler icon catalog (loaded from assets/fonts/tabler.json on first registry use).
  const std::unordered_map<std::string, char32_t>& tablerIcons();
  std::optional<std::string_view> categoryFor(std::string_view name);
  // Hand-curated Noctalia alias -> native Tabler icon name map.
  const std::unordered_map<std::string, std::string_view>& aliases();

} // namespace GlyphRegistry
