#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

namespace StringUtils {

  [[nodiscard]] inline std::string_view trimLeftView(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) {
      s.remove_prefix(1);
    }
    return s;
  }

  [[nodiscard]] inline std::string_view trimRightView(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) {
      s.remove_suffix(1);
    }
    return s;
  }

  [[nodiscard]] inline std::string trim(std::string_view s) { return std::string(trimRightView(trimLeftView(s))); }

  // Window titles may contain embedded newlines, collapse to a single display/match line.
  [[nodiscard]] inline std::string windowTitleSingleLine(std::string_view text) {
    if (text.empty()) {
      return {};
    }

    std::string out;
    out.reserve(text.size());
    bool pendingSpace = false;
    for (unsigned char ch : text) {
      if (ch == '\n' || ch == '\r' || ch == '\t' || ch == '\v' || ch == '\f' || ch == ' ' || std::isspace(ch) != 0) {
        pendingSpace = !out.empty();
        continue;
      }
      if (pendingSpace) {
        out.push_back(' ');
        pendingSpace = false;
      }
      out.push_back(static_cast<char>(ch));
    }
    return out;
  }

  template <typename T> [[nodiscard]] inline std::optional<T> parseDotDecimal(std::string_view text) {
    static_assert(std::is_floating_point_v<T>);

    const std::string trimmed = trim(text);
    if (trimmed.empty()) {
      return std::nullopt;
    }

    T value{};
    const char* begin = trimmed.data();
    const char* end = begin + trimmed.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value, std::chars_format::general);
    if (ec != std::errc{} || ptr != end || !std::isfinite(value)) {
      return std::nullopt;
    }
    return value;
  }

  [[nodiscard]] inline std::string formatDotDecimal(double value) {
    std::array<char, 64> buffer{};
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc{}) {
      return {};
    }
    return std::string(buffer.data(), ptr);
  }

  [[nodiscard]] inline std::string formatFixedDotDecimal(double value, int precision) {
    std::array<char, 64> buffer{};
    const auto [ptr, ec] =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::fixed, precision);
    if (ec != std::errc{}) {
      return {};
    }
    return std::string(buffer.data(), ptr);
  }

  [[nodiscard]] inline std::string toLower(std::string_view s) {
    std::string out(s);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
  }

  [[nodiscard]] inline std::string pathTail(std::string_view path) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string_view::npos || slash + 1 >= path.size()) {
      return std::string(path);
    }
    return std::string(path.substr(slash + 1));
  }

  [[nodiscard]] inline std::string urlEncode(std::string_view text) {
    constexpr char kHexDigits[] = "0123456789ABCDEF";

    std::string encoded;
    encoded.reserve(text.size() * 3);

    auto isUnreserved = [](unsigned char ch) {
      return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '~';
    };

    for (char rawCh : text) {
      const auto ch = static_cast<unsigned char>(rawCh);
      if (isUnreserved(ch)) {
        encoded.push_back(rawCh);
      } else {
        encoded.push_back('%');
        encoded.push_back(kHexDigits[static_cast<std::size_t>(ch >> 4U)]);
        encoded.push_back(kHexDigits[static_cast<std::size_t>(ch & 0x0FU)]);
      }
    }

    return encoded;
  }

  [[nodiscard]] inline std::string urlDecode(std::string_view text) {
    auto hexValue = [](unsigned char ch) -> int {
      if (ch >= '0' && ch <= '9') {
        return ch - '0';
      }
      if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
      }
      if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
      }
      return -1;
    };

    std::string decoded;
    decoded.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
      const char ch = text[i];
      if (ch == '%' && i + 2 < text.size()) {
        const int hi = hexValue(static_cast<unsigned char>(text[i + 1]));
        const int lo = hexValue(static_cast<unsigned char>(text[i + 2]));
        if (hi >= 0 && lo >= 0) {
          decoded.push_back(static_cast<char>((hi << 4) | lo));
          i += 2;
          continue;
        }
      }
      if (ch == '+') {
        decoded.push_back(' ');
        continue;
      }
      decoded.push_back(ch);
    }
    return decoded;
  }

  inline void toLowerInPlace(std::string& s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  }

  [[nodiscard]] inline bool containsInsensitive(std::string_view haystack, std::string_view needle) {
    if (haystack.empty() || needle.empty()) {
      return false;
    }
    std::string lhs(haystack);
    std::string rhs(needle);
    toLowerInPlace(lhs);
    toLowerInPlace(rhs);
    return lhs.contains(rhs);
  }

  [[nodiscard]] inline std::string trimLeadingBlankLines(std::string_view text) {
    if (text.empty()) {
      return {};
    }

    std::size_t start = 0;
    while (start < text.size()) {
      std::size_t lineEnd = text.find('\n', start);
      if (lineEnd == std::string_view::npos) {
        lineEnd = text.size();
      }
      const std::string_view line = text.substr(start, lineEnd - start);
      const bool blankLine =
          line.empty() || std::ranges::all_of(line, [](unsigned char ch) { return std::isspace(ch) != 0; });
      if (!blankLine) {
        break;
      }
      if (lineEnd >= text.size()) {
        start = text.size();
        break;
      }
      start = lineEnd + 1;
    }

    return std::string(text.substr(start));
  }

  [[nodiscard]] inline std::string truncateByLines(std::string_view text, int maxLines, bool* didTruncate = nullptr) {
    if (didTruncate != nullptr) {
      *didTruncate = false;
    }
    if (maxLines <= 0 || text.empty()) {
      return std::string(text);
    }

    int seenLines = 1;
    std::size_t index = 0;
    while (index < text.size()) {
      if (text[index] == '\n') {
        ++seenLines;
        if (seenLines > maxLines) {
          if (didTruncate != nullptr) {
            *didTruncate = true;
          }
          return std::string(text.substr(0, index));
        }
      }
      ++index;
    }
    return std::string(text);
  }

  // True when `text` is exactly one codepoint in a Unicode Private Use Area —
  // i.e. a Nerd Font / icon glyph rather than text. Such glyphs ignore cap/x
  // height, so callers center them by ink instead of the typographic cap band.
  [[nodiscard]] inline bool isSinglePrivateUseGlyph(std::string_view text) {
    if (text.empty()) {
      return false;
    }
    const auto lead = static_cast<unsigned char>(text[0]);
    std::size_t len = 0;
    char32_t cp = 0;
    if (lead < 0x80) {
      len = 1;
      cp = lead;
    } else if ((lead & 0xE0) == 0xC0) {
      len = 2;
      cp = lead & 0x1Fu;
    } else if ((lead & 0xF0) == 0xE0) {
      len = 3;
      cp = lead & 0x0Fu;
    } else if ((lead & 0xF8) == 0xF0) {
      len = 4;
      cp = lead & 0x07u;
    } else {
      return false;
    }
    if (text.size() != len) {
      return false; // more than one codepoint, or a truncated sequence
    }
    for (std::size_t i = 1; i < len; ++i) {
      const auto byte = static_cast<unsigned char>(text[i]);
      if ((byte & 0xC0) != 0x80) {
        return false;
      }
      cp = (cp << 6) | (byte & 0x3Fu);
    }
    return (cp >= 0xE000 && cp <= 0xF8FF) || (cp >= 0xF0000 && cp <= 0xFFFFD) || (cp >= 0x100000 && cp <= 0x10FFFD);
  }

  [[nodiscard]] inline std::string truncateUtf8CodePoints(std::string_view text, std::size_t maxCodePoints) {
    std::size_t codePoints = 0;
    std::size_t bytePos = 0;
    while (bytePos < text.size() && codePoints < maxCodePoints) {
      auto lead = static_cast<unsigned char>(text[bytePos]);
      if (lead < 0x80)
        bytePos += 1;
      else if ((lead & 0xE0) == 0xC0)
        bytePos += 2;
      else if ((lead & 0xF0) == 0xE0)
        bytePos += 3;
      else
        bytePos += 4;
      ++codePoints;
    }
    if (bytePos >= text.size()) {
      return std::string(text);
    }
    return std::string(text.substr(0, bytePos));
  }

  [[nodiscard]] inline std::string truncateUtf8(std::string_view text, std::size_t maxBytes) {
    if (text.size() <= maxBytes) {
      return std::string(text);
    }

    std::size_t end = maxBytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
      --end;
    }
    return std::string(text.substr(0, end));
  }

  // Strip HTML/Pango tags and unescape XML entities.
  [[nodiscard]] inline std::string sanitizeMarkup(std::string_view s) {
    std::string out;
    out.reserve(s.size());

    size_t i = 0;
    while (i < s.size()) {
      if (s[i] == '<') {
        size_t close = s.find('>', i + 1);
        if (close != std::string_view::npos) {
          auto tag = toLower(s.substr(i + 1, close - i - 1));
          if (tag == "br" || tag == "br/" || tag == "br /") {
            out += '\n';
          }
          i = close + 1;
          continue;
        }
      }

      if (s[i] == '&') {
        std::string_view rest = s.substr(i);
        if (rest.starts_with("&lt;")) {
          out += '<';
          i += 4;
        } else if (rest.starts_with("&gt;")) {
          out += '>';
          i += 4;
        } else if (rest.starts_with("&amp;")) {
          out += '&';
          i += 5;
        } else if (rest.starts_with("&quot;")) {
          out += '"';
          i += 6;
        } else if (rest.starts_with("&apos;")) {
          out += '\'';
          i += 6;
        } else {
          out += s[i];
          ++i;
        }
      } else {
        out += s[i];
        ++i;
      }
    }

    return out;
  }

  [[nodiscard]] inline std::vector<std::string> splitWhitespace(std::string_view text) {
    std::vector<std::string> result;
    std::size_t i = 0;
    while (i < text.size()) {
      while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) {
        ++i;
      }
      if (i >= text.size()) {
        break;
      }
      std::size_t start = i;
      while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) == 0) {
        ++i;
      }
      result.emplace_back(text.substr(start, i - start));
    }
    return result;
  }

  [[nodiscard]] inline std::vector<std::string_view> split(std::string_view text, char delimiter) {
    std::vector<std::string_view> result;
    std::size_t start = 0;
    while (start <= text.size()) {
      std::size_t pos = text.find(delimiter, start);
      if (pos == std::string_view::npos) {
        result.push_back(text.substr(start));
        break;
      }
      result.push_back(text.substr(start, pos - start));
      start = pos + 1;
    }
    return result;
  }

  [[nodiscard]] inline std::string join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string result;
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (i > 0) {
        result.append(separator);
      }
      result.append(parts[i]);
    }
    return result;
  }

  [[nodiscard]] inline std::string replaceAll(std::string_view input, std::string_view from, std::string_view to) {
    if (from.empty()) {
      return std::string(input);
    }
    std::string result;
    result.reserve(input.size());
    std::size_t pos = 0;
    while (pos < input.size()) {
      std::size_t found = input.find(from, pos);
      if (found == std::string_view::npos) {
        result.append(input.substr(pos));
        break;
      }
      result.append(input.substr(pos, found - pos));
      result.append(to);
      pos = found + from.size();
    }
    return result;
  }

  [[nodiscard]] inline bool isBlank(std::string_view text) {
    return text.empty() || std::ranges::all_of(text, [](unsigned char ch) { return std::isspace(ch) != 0; });
  }

  [[nodiscard]] inline std::string shellQuote(std::string_view text) {
    std::string result = "'";
    for (char ch : text) {
      if (ch == '\'') {
        result += "'\\''";
      } else {
        result += ch;
      }
    }
    result += '\'';
    return result;
  }

  [[nodiscard]] inline std::string quoteDouble(std::string_view text) {
    std::string result = "\"";
    for (char ch : text) {
      if (ch == '\\' || ch == '"') {
        result += '\\';
      }
      result += ch;
    }
    result += '"';
    return result;
  }

  [[nodiscard]] inline std::string unquote(std::string_view text) {
    if (text.size() < 2) {
      return std::string(text);
    }
    char front = text.front();
    char back = text.back();
    if ((front == '"' && back == '"') || (front == '\'' && back == '\'')) {
      text = text.substr(1, text.size() - 2);
      std::string result;
      result.reserve(text.size());
      for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
          ++i;
        }
        result += text[i];
      }
      return result;
    }
    return std::string(text);
  }

  [[nodiscard]] inline std::string snakeToKebab(std::string_view s) {
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
      result += (c == '_') ? '-' : c;
    }
    return result;
  }

  [[nodiscard]] inline std::string generateUuid() {
    std::uint8_t bytes[16]{};
    FILE* urandom = std::fopen("/dev/urandom", "rb");
    if (urandom == nullptr) {
      return {};
    }
    const std::size_t read = std::fread(bytes, 1, sizeof(bytes), urandom);
    std::fclose(urandom);
    if (read != sizeof(bytes)) {
      return {};
    }
    bytes[6] = (bytes[6] & 0x0Fu) | 0x40u;
    bytes[8] = (bytes[8] & 0x3Fu) | 0x80u;
    return std::format(
        "{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-"
        "{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10],
        bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]
    );
  }
} // namespace StringUtils
