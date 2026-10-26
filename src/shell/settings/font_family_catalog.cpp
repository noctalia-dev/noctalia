#include "shell/settings/font_family_catalog.h"

#include "core/process/process.h"
#include "i18n/i18n.h"
#include "util/string_utils.h"

#include <algorithm>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace settings {
  namespace {

    std::vector<std::string> discoverFontFamiliesUncached() {
      if (!process::commandExists("fc-list")) {
        return {};
      }

      const auto result = process::runSync({"fc-list", ":", "family"});
      if (!result) {
        return {};
      }

      return parseFontFamilyList(result.out);
    }

  } // namespace

  std::vector<std::string> parseFontFamilyList(std::string_view output) {
    std::unordered_set<std::string> seen;
    seen.reserve(4096);

    std::string token;
    const auto flush = [&] {
      std::string family = StringUtils::trim(token);
      if (!family.empty()) {
        seen.insert(std::move(family));
      }
      token.clear();
    };

    // fontconfig escapes `\`, `-`, `:` and `,` inside values with a backslash; unescaped commas
    // separate the families of one font and newlines separate fonts.
    for (std::size_t i = 0; i < output.size(); ++i) {
      const char c = output[i];
      if (c == '\\' && i + 1 < output.size()) {
        token += output[++i];
      } else if (c == ',' || c == '\n') {
        flush();
      } else {
        token += c;
      }
    }
    flush();

    std::vector<std::string> families(seen.begin(), seen.end());
    std::ranges::sort(families, [](const std::string& a, const std::string& b) {
      return StringUtils::toLower(a) < StringUtils::toLower(b);
    });
    return families;
  }

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
