#include "calendar/event_link.h"

#include "util/string_utils.h"

#include <array>
#include <cctype>
#include <cstddef>

namespace calendar {
  namespace {
    constexpr std::size_t kMaxLinkLength = 2048;
    constexpr std::string_view kWhitespace = " \t\r\n\v\f";

    bool equalsIgnoreCase(std::string_view a, std::string_view b) {
      if (a.size() != b.size()) {
        return false;
      }
      for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
          return false;
        }
      }
      return true;
    }

    // Length of the http(s) scheme prefix at the start of `text`, or 0 when it starts with neither.
    std::size_t schemePrefixLength(std::string_view text) {
      for (const std::string_view scheme : std::array<std::string_view, 2>{"https://", "http://"}) {
        if (text.size() > scheme.size() && equalsIgnoreCase(text.substr(0, scheme.size()), scheme)) {
          return scheme.size();
        }
      }
      return 0;
    }

    bool hasHost(std::string_view link, std::size_t schemeLength) {
      const std::string_view rest = link.substr(schemeLength);
      return rest.find_first_of("/?#") != 0;
    }

    bool isValidLink(std::string_view link) {
      if (link.empty() || link.size() > kMaxLinkLength) {
        return false;
      }
      const std::size_t schemeLength = schemePrefixLength(link);
      if (schemeLength == 0 || !hasHost(link, schemeLength)) {
        return false;
      }
      for (const char c : link) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte == 0x7f) {
          return false;
        }
      }
      return true;
    }

    // Drop sentence punctuation and unbalanced closing wrappers a link picks up when it is embedded
    // in prose, e.g. "Join: <https://meet.example/x>." or "(https://meet.example/x)".
    std::string_view trimTrailingPunctuation(std::string_view candidate) {
      while (!candidate.empty()) {
        const char last = candidate.back();
        const bool sentencePunctuation =
            last == '.' || last == ',' || last == ';' || last == ':' || last == '!' || last == '?';
        const bool unbalancedWrapper = (last == ')' && !candidate.contains('('))
            || (last == ']' && !candidate.contains('['))
            || (last == '}' && !candidate.contains('{'))
            || last == '>'
            || last == '"'
            || last == '\'';
        if (!sentencePunctuation && !unbalancedWrapper) {
          break;
        }
        candidate.remove_suffix(1);
      }
      return candidate;
    }

    std::string extractEmbeddedLink(std::string_view text) {
      for (std::size_t i = 0; i < text.size(); ++i) {
        if (schemePrefixLength(text.substr(i)) == 0) {
          continue;
        }
        // Only match at a token boundary, so "myhttp://x" is not read as a link.
        if (i > 0 && std::isalnum(static_cast<unsigned char>(text[i - 1])) != 0) {
          continue;
        }
        std::string_view candidate = text.substr(i);
        if (const std::size_t end = candidate.find_first_of(kWhitespace); end != std::string_view::npos) {
          candidate = candidate.substr(0, end);
        }
        candidate = trimTrailingPunctuation(candidate);
        if (isValidLink(candidate)) {
          return std::string(candidate);
        }
      }
      return {};
    }
  } // namespace

  std::string resolveEventLink(std::string_view location, std::string_view urlProperty) {
    if (std::string link = extractEmbeddedLink(location); !link.empty()) {
      return link;
    }
    const std::string_view trimmed = StringUtils::trimRightView(StringUtils::trimLeftView(urlProperty));
    return isValidLink(trimmed) ? std::string(trimmed) : std::string{};
  }

} // namespace calendar
