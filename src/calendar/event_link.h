#pragma once

#include <string>
#include <string_view>

namespace calendar {

  // Resolve the clickable link for an event. Meeting links are conventionally written into LOCATION
  // by Google and Outlook, while the RFC 5545 URL property more often points at an event page, so a
  // link embedded in `location` wins over `urlProperty`.
  // Only http(s) links survive: the value comes from a remote calendar server and is handed to
  // xdg-open, so schemes such as file: or data: must never reach a handler. Returns empty when
  // neither input yields a valid link.
  [[nodiscard]] std::string resolveEventLink(std::string_view location, std::string_view urlProperty);

} // namespace calendar
