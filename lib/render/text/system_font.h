#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace text {

  // The desktop's preferred UI font, from GTK settings: GSettings org.gnome.desktop.interface
  // font-name (only when the user set it), else gtk-font-name in gtk-3.0 then gtk-4.0 settings.ini.
  struct SystemFont {
    std::string family;             // empty = not configured
    float sizePx = 0.0F;            // 0 = not configured
    float textScalingFactor = 1.0F; // GNOME "Large Text" (org.gnome.desktop.interface text-scaling-factor)

    bool operator==(const SystemFont&) const = default;

    // Factor for all text: (sizePx, or referenceBodyPx when unset) * textScalingFactor / referenceBodyPx.
    [[nodiscard]] float textScale(float referenceBodyPx) const noexcept;
  };

  // Reads the sources now; no caching.
  [[nodiscard]] SystemFont readSystemFont();

  // Last value stored by refreshSystemFont() (empty until the first refresh).
  [[nodiscard]] const SystemFont& currentSystemFont() noexcept;

  // Re-reads the sources and stores the result. Returns true if it changed.
  bool refreshSystemFont();

  // `configured` (trimmed) if non-empty, else the system font family, else "sans-serif".
  [[nodiscard]] std::string resolveFontFamily(std::string_view configured);

  // Files whose changes may change readSystemFont(), for a FileWatcher: the GTK settings.ini files
  // and the dconf user database behind GSettings (only those whose directory exists), plus the
  // targets of symlinked settings.ini files.
  [[nodiscard]] std::vector<std::filesystem::path> systemFontWatchFiles();

} // namespace text
