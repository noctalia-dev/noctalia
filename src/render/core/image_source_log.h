#pragma once

#include "util/string_utils.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace ImageSourceLog {

  namespace detail {

    constexpr std::size_t kMaxLoggedSourceBytes = 512;
    constexpr std::size_t kMaxLoggedDataHeaderBytes = 96;

    inline bool asciiStartsWithDataScheme(std::string_view source) {
      if (source.size() < 5) {
        return false;
      }

      return (source[0] == 'd' || source[0] == 'D')
          && (source[1] == 'a' || source[1] == 'A')
          && (source[2] == 't' || source[2] == 'T')
          && (source[3] == 'a' || source[3] == 'A')
          && source[4] == ':';
    }

    inline std::string truncatedWithSize(std::string_view value, std::size_t maxBytes) {
      if (value.size() <= maxBytes) {
        return std::string(value);
      }

      return StringUtils::truncateUtf8(value, maxBytes)
          + " ... [truncated, original="
          + std::to_string(value.size())
          + " bytes]";
    }

  } // namespace detail

  inline std::string describe(std::string_view source) {
    if (!detail::asciiStartsWithDataScheme(source)) {
      return detail::truncatedWithSize(source, detail::kMaxLoggedSourceBytes);
    }

    const std::size_t comma = source.find(',');
    const std::string_view header =
        source.substr(5, comma == std::string_view::npos ? std::string_view::npos : comma - 5);
    std::string result = "data:";
    result += header.empty() ? "<no-media-type>" : detail::truncatedWithSize(header, detail::kMaxLoggedDataHeaderBytes);

    if (comma == std::string_view::npos) {
      result += " (malformed, uri=";
      result += std::to_string(source.size());
      result += " bytes)";
      return result;
    }

    result += " (payload=";
    result += std::to_string(source.size() - comma - 1);
    result += " bytes, uri=";
    result += std::to_string(source.size());
    result += " bytes)";
    return result;
  }

} // namespace ImageSourceLog
