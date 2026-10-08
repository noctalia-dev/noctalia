#pragma once

#include <string>
#include <string_view>
#include <vector>

// Shared naming for panel ids ("clipboard", "author/plugin:entry"): used by both
// the launcher's panel provider and the settings ignore-list picker so they agree
// on what a panel is called.
namespace panel_catalog {

  struct Description {
    std::string title;
    std::string subtitle;
    std::string glyph;
  };

  // Resolves a raw panel id to a display title/subtitle/glyph: a small built-in
  // table (core panels ship no manifest to read one from), the owning plugin's
  // manifest name/icon (entry id as subtitle), or the raw id as a last resort.
  [[nodiscard]] Description describe(std::string_view panelId);

  // Every panel id that could be registered: the built-in table plus every
  // [[panel]] entry of a currently-active plugin (live via PluginRegistry).
  [[nodiscard]] std::vector<std::string> allKnownIds();

} // namespace panel_catalog
