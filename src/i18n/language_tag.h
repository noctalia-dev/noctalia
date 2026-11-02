#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace i18n::detail {

  std::string normalizeLanguageTag(std::string_view raw);
  std::vector<std::string> catalogLanguageCandidates(std::string_view raw);
  bool isRtlLanguage(std::string_view tag);

} // namespace i18n::detail
