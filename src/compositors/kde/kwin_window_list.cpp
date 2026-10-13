#include "compositors/kde/kwin_window_list.h"

#include "util/string_utils.h"

#include <string_view>
#include <vector>

namespace {

  constexpr char kRecordSeparator = '\x1F';
  constexpr char kFieldSeparator = '\x1e';

  [[nodiscard]] bool isNoctaliaShellSurface(const std::string& appId, const std::string& title) {
    const std::string appLower = StringUtils::toLower(appId);
    if (appLower == "noctalia") {
      return true;
    }
    return appLower.empty() && StringUtils::toLower(title) == "noctalia";
  }

  [[nodiscard]] std::vector<std::string> splitString(std::string_view value, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= value.size()) {
      const std::size_t end = value.find(separator, start);
      if (end == std::string_view::npos) {
        parts.emplace_back(value.substr(start));
        break;
      }
      parts.emplace_back(value.substr(start, end - start));
      start = end + 1;
    }
    return parts;
  }

} // namespace

namespace compositors::kde {

  std::vector<KwinTrackedWindow> parseWindowListPayload(std::string_view payload) {
    std::vector<KwinTrackedWindow> windows;
    for (const auto& record : splitString(payload, kRecordSeparator)) {
      if (record.empty()) {
        continue;
      }
      const auto fields = splitString(record, kFieldSeparator);
      if (fields.size() < 3) {
        continue;
      }
      KwinTrackedWindow window{
          .uuid = fields[0],
          .appId = fields[1],
          .title = StringUtils::windowTitleSingleLine(fields[2]),
          .outputName = fields.size() >= 5 ? fields[4] : std::string{},
          .desktopIds = {},
          .minimized = fields.size() >= 6 && fields[5] == "1",
      };
      if (fields.size() >= 4 && !fields[3].empty() && fields[3] != "*") {
        window.desktopIds = splitString(fields[3], ',');
      }
      if (window.uuid.empty() && window.appId.empty()) {
        continue;
      }
      if (isNoctaliaShellSurface(window.appId, window.title)) {
        continue;
      }
      windows.push_back(std::move(window));
    }
    return windows;
  }

} // namespace compositors::kde
