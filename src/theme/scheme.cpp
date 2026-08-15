#include "theme/scheme.h"

namespace noctalia::theme {

  std::optional<Scheme> schemeFromString(std::string_view s) {
    for (const auto& [name, scheme] : kSchemeEntries) {
      if (name == s)
        return scheme;
    }
    return std::nullopt;
  }

  std::string_view schemeToString(Scheme s) {
    for (const auto& [name, scheme] : kSchemeEntries) {
      if (scheme == s)
        return name;
    }
    return kSchemeEntries.front().first;
  }

} // namespace noctalia::theme
