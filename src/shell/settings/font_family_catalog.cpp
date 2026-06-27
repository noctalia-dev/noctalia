#include "shell/settings/font_family_catalog.h"

#include "core/process.h"
#include "i18n/i18n.h"
#include "util/string_utils.h"

#include <algorithm>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace settings {
  namespace {

    std::vector<std::string> discoverFontFamiliesUncached() {
      std::vector<std::string> families;
      if (!process::commandExists("fc-list")) {
        return families;
      }

      const auto result = process::runSync({"fc-list", ":", "family"});
      if (!result) {
        return families;
      }

      std::unordered_set<std::string> seen;
      seen.reserve(4096);

      std::size_t lineStart = 0;
      while (lineStart <= result.out.size()) {
        const std::size_t lineEnd = result.out.find('\n', lineStart);
        const std::string_view line = lineEnd == std::string::npos
            ? std::string_view(result.out).substr(lineStart)
            : std::string_view(result.out).substr(lineStart, lineEnd - lineStart);

        std::size_t tokenStart = 0;
        while (tokenStart <= line.size()) {
          const std::size_t tokenEnd = line.find(',', tokenStart);
          const std::string_view token =
              tokenEnd == std::string::npos ? line.substr(tokenStart) : line.substr(tokenStart, tokenEnd - tokenStart);
          std::string family = StringUtils::trim(std::string(token));
          if (!family.empty()) {
            seen.insert(std::move(family));
          }
          if (tokenEnd == std::string::npos) {
            break;
          }
          tokenStart = tokenEnd + 1;
        }

        if (lineEnd == std::string::npos) {
          break;
        }
        lineStart = lineEnd + 1;
      }

      families.assign(seen.begin(), seen.end());
      std::ranges::sort(families, [](const std::string& a, const std::string& b) {
        return StringUtils::toLower(a) < StringUtils::toLower(b);
      });
      return families;
    }

  } // namespace

  const std::vector<std::string>& discoverFontFamilies() {
    static const std::vector<std::string> kFamilies = discoverFontFamiliesUncached();
    return kFamilies;
  }

  std::vector<WidgetSettingSelectOption> buildFontFamilySelectOptions() {
    const std::vector<std::string>& families = discoverFontFamilies();

    std::vector<WidgetSettingSelectOption> options;
    options.reserve(families.size() + 1);
    options.push_back(WidgetSettingSelectOption{"", i18n::tr("desktop-widgets.editor.settings.font-family-default")});
    for (const std::string& family : families) {
      options.push_back(WidgetSettingSelectOption{family, family});
    }
    return options;
  }

} // namespace settings
