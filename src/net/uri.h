#pragma once

#include <string>
#include <string_view>

namespace uri {

  std::string decodeComponent(std::string_view text);
  // Percent-encodes a string for use as a URL path segment or query value, leaving only the RFC 3986
  // unreserved set (A-Z a-z 0-9 - _ . ~) intact.
  std::string encodeComponent(std::string_view text);
  bool isRemoteUrl(std::string_view url);
  std::string normalizeFileUrl(std::string_view url);

} // namespace uri
