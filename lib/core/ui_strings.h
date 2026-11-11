#pragma once

#include <functional>
#include <string>
#include <string_view>

// User-visible text the library shows on its own. The library owns no translations: the application
// installs these providers at startup and resolves them through its own catalog. Providers are called
// whenever the text is needed, so a language change takes effect for text built afterwards.
struct UiStrings {
  // Display name of a key without a printable glyph, given its XKB keysym name ("Return", "Prior").
  // Empty result, or no provider: the XKB name is shown.
  std::function<std::string(std::string_view xkbKeyName)> keyName;
  // Text a Select shows while nothing is selected and the caller set no placeholder.
  // No provider: the Select shows nothing.
  std::function<std::string()> selectPlaceholder;
};

void setUiStrings(UiStrings strings);
[[nodiscard]] const UiStrings& uiStrings();
