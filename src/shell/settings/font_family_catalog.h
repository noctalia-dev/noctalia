#pragma once

#include "shell/settings/widget_settings_registry.h"

#include <string>
#include <string_view>
#include <vector>

namespace settings {

  // Parses `fc-list : family` output into unescaped families, deduplicated and sorted case-insensitively.
  [[nodiscard]] std::vector<std::string> parseFontFamilyList(std::string_view output);

  // Installed font families, deduplicated and sorted case-insensitively. Discovered once via
  // `fc-list` and cached for the process lifetime.
  [[nodiscard]] const std::vector<std::string>& discoverFontFamilies();

  // Select options for a per-widget font picker: an inherit/default entry (empty value) followed by
  // every installed family. Option labels are literal font names (not i18n keys), so the owning spec
  // must set `literalLabels = true`.
  [[nodiscard]] std::vector<WidgetSettingSelectOption> buildFontFamilySelectOptions();

} // namespace settings
