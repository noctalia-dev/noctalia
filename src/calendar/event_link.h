#pragma once

#include <string>
#include <string_view>

namespace calendar {

  // Resolve the clickable link from event fields. LOCATION conventionally carries a meeting link;
  // callers put another meeting-specific field such as DESCRIPTION or hangoutLink second, followed
  // by the generic URL property. Only http(s) links survive: remote calendar data is handed to
  // xdg-open, so schemes such as file: or data: must never reach a handler. Returns empty when no
  // field yields a valid link.
  [[nodiscard]] std::string
  resolveEventLink(std::string_view location, std::string_view preferredLink, std::string_view urlProperty);

} // namespace calendar
