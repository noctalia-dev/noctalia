#pragma once

#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace scripting {

  [[nodiscard]] inline bool isValidPluginIdSegment(std::string_view segment) {
    if (segment.empty()) {
      return false;
    }
    const auto first = static_cast<unsigned char>(segment.front());
    if (std::isalnum(first) == 0) {
      return false;
    }
    for (const char ch : segment) {
      const auto c = static_cast<unsigned char>(ch);
      if (std::isalnum(c) != 0 || ch == '_' || ch == '-' || ch == '.') {
        continue;
      }
      return false;
    }
    return segment != "." && segment != "..";
  }

  [[nodiscard]] inline bool isValidPluginId(std::string_view id) {
    const std::size_t slash = id.find('/');
    if (slash == std::string_view::npos || id.find('/', slash + 1) != std::string_view::npos) {
      return false;
    }
    return isValidPluginIdSegment(id.substr(0, slash)) && isValidPluginIdSegment(id.substr(slash + 1));
  }

  // Repo subdir for a plugin id by convention: "author/foo" lives at "foo/".
  [[nodiscard]] inline std::optional<std::string> pluginSubdirFromId(std::string_view id) {
    if (!isValidPluginId(id)) {
      return std::nullopt;
    }
    return std::string(id.substr(id.find('/') + 1));
  }

  // Public plugin page on noctalia.dev for official/community catalog entries.
  [[nodiscard]] inline std::optional<std::string> pluginWebsitePageUrl(std::string_view source, std::string_view id) {
    if (source != "official" && source != "community") {
      return std::nullopt;
    }
    const auto subdir = pluginSubdirFromId(id);
    if (!subdir.has_value()) {
      return std::nullopt;
    }
    return "https://noctalia.dev/plugins/" + std::string(source) + "/" + *subdir;
  }

} // namespace scripting
