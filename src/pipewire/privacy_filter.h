#pragma once

#include <optional>
#include <regex>
#include <string>
#include <string_view>

// Capture-name regex filter shared by the privacy bar widget and the privacy
// OSD. Recompiles only when the pattern string actually changes; an empty
// pattern matches nothing.
class PrivacyFilter {
public:
  // Recompiles the regex if pattern differs from the current one. key names the
  // config field for diagnostics on an invalid pattern.
  void update(std::string_view key, const std::string& pattern);
  [[nodiscard]] bool matches(const std::string& value) const;

private:
  std::string m_pattern;
  std::optional<std::regex> m_regex;
};
