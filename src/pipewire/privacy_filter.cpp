#include "pipewire/privacy_filter.h"

#include "core/log.h"

namespace {
  constexpr Logger kLog("privacy");
}

void PrivacyFilter::update(std::string_view key, const std::string& pattern) {
  if (pattern == m_pattern) {
    return;
  }
  m_pattern = pattern;
  if (pattern.empty()) {
    m_regex.reset();
    return;
  }
  try {
    m_regex = std::regex(pattern);
  } catch (const std::regex_error& e) {
    kLog.warn("invalid {} '{}': {}", key, pattern, e.what());
    m_regex.reset();
  }
}

bool PrivacyFilter::matches(const std::string& value) const {
  return m_regex.has_value() && std::regex_search(value, *m_regex);
}
