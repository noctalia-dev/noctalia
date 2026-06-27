#include "i18n/language_tag.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace i18n::detail {

  namespace {

    [[nodiscard]] bool isAlpha(std::string_view value) {
      return std::ranges::all_of(value, [](unsigned char c) { return std::isalpha(c) != 0; });
    }

    [[nodiscard]] bool isDigit(std::string_view value) {
      return std::ranges::all_of(value, [](unsigned char c) { return std::isdigit(c) != 0; });
    }

    [[nodiscard]] std::string lower(std::string_view value) {
      std::string out(value);
      for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      return out;
    }

    [[nodiscard]] std::string upper(std::string_view value) {
      std::string out(value);
      for (char& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      }
      return out;
    }

    [[nodiscard]] std::string title(std::string_view value) {
      std::string out = lower(value);
      if (!out.empty()) {
        out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
      }
      return out;
    }

    [[nodiscard]] bool isScriptSubtag(std::string_view subtag) { return subtag.size() == 4 && isAlpha(subtag); }

    [[nodiscard]] bool isRegionSubtag(std::string_view subtag) {
      return (subtag.size() == 2 && isAlpha(subtag)) || (subtag.size() == 3 && isDigit(subtag));
    }

    [[nodiscard]] std::vector<std::string_view> split(std::string_view tag) {
      std::vector<std::string_view> parts;
      std::size_t start = 0;
      while (start <= tag.size()) {
        const std::size_t end = tag.find('-', start);
        const std::size_t length = end == std::string_view::npos ? tag.size() - start : end - start;
        parts.push_back(tag.substr(start, length));
        if (end == std::string_view::npos) {
          break;
        }
        start = end + 1;
      }
      return parts;
    }

    [[nodiscard]] std::string join(const std::vector<std::string>& parts) {
      std::string out;
      for (const std::string& part : parts) {
        if (!out.empty()) {
          out += '-';
        }
        out += part;
      }
      return out;
    }

    void appendUnique(std::vector<std::string>& out, std::string value) {
      if (value.empty()) {
        return;
      }
      if (!std::ranges::contains(out, value)) {
        out.push_back(std::move(value));
      }
    }

    struct ParsedTag {
      std::vector<std::string> parts;
      std::string language;
      std::string script;
      std::string region;
      std::size_t scriptIndex = 0;
    };

    [[nodiscard]] ParsedTag parseTag(std::string_view tag) {
      ParsedTag parsed;
      for (std::string_view part : split(tag)) {
        parsed.parts.emplace_back(part);
      }
      if (parsed.parts.empty()) {
        return parsed;
      }

      parsed.language = parsed.parts.front();
      for (std::size_t i = 1; i < parsed.parts.size(); ++i) {
        const std::string& part = parsed.parts[i];
        if (parsed.script.empty() && isScriptSubtag(part)) {
          parsed.script = part;
          parsed.scriptIndex = i;
        } else if (parsed.region.empty() && isRegionSubtag(part)) {
          parsed.region = part;
        }
      }
      return parsed;
    }

    [[nodiscard]] std::string inferredChineseScript(std::string_view language, std::string_view region) {
      if (language != "zh") {
        return {};
      }

      constexpr std::array<std::string_view, 3> simplifiedRegions = {"CN", "SG", "MY"};
      constexpr std::array<std::string_view, 3> traditionalRegions = {"TW", "HK", "MO"};
      if (std::ranges::contains(simplifiedRegions, region)) {
        return "Hans";
      }
      if (std::ranges::contains(traditionalRegions, region)) {
        return "Hant";
      }
      return {};
    }

    [[nodiscard]] std::string withInsertedScript(const ParsedTag& parsed, std::string_view script) {
      if (script.empty()) {
        return {};
      }
      std::vector<std::string> parts = parsed.parts;
      parts.insert(parts.begin() + 1, std::string(script));
      return join(parts);
    }

    [[nodiscard]] std::string withoutScript(const ParsedTag& parsed) {
      if (parsed.scriptIndex == 0) {
        return {};
      }
      std::vector<std::string> parts = parsed.parts;
      parts.erase(parts.begin() + static_cast<std::ptrdiff_t>(parsed.scriptIndex));
      return join(parts);
    }

    [[nodiscard]] std::string scriptOnly(const ParsedTag& parsed, std::string_view script) {
      if (script.empty()) {
        return {};
      }
      return parsed.language + "-" + std::string(script);
    }

  } // namespace

  std::string normalizeLanguageTag(std::string_view raw) {
    std::string stripped(raw);
    if (auto pos = stripped.find('.'); pos != std::string::npos) {
      stripped.resize(pos);
    }
    if (auto pos = stripped.find('@'); pos != std::string::npos) {
      stripped.resize(pos);
    }
    for (char& c : stripped) {
      if (c == '_') {
        c = '-';
      }
    }

    if (stripped.empty() || stripped == "C" || stripped == "POSIX") {
      return {};
    }

    std::vector<std::string_view> rawParts = split(stripped);
    if (rawParts.empty() || rawParts.front().empty()) {
      return {};
    }

    std::vector<std::string> parts;
    parts.reserve(rawParts.size());
    parts.push_back(lower(rawParts.front()));
    for (std::size_t i = 1; i < rawParts.size(); ++i) {
      const std::string_view part = rawParts[i];
      if (part.empty()) {
        return {};
      }
      if (isScriptSubtag(part)) {
        parts.push_back(title(part));
      } else if (isRegionSubtag(part)) {
        parts.push_back(upper(part));
      } else {
        parts.push_back(lower(part));
      }
    }

    return join(parts);
  }

  std::vector<std::string> catalogLanguageCandidates(std::string_view raw) {
    std::vector<std::string> candidates;

    const std::string normalized = normalizeLanguageTag(raw);
    if (normalized.empty()) {
      return candidates;
    }

    const ParsedTag parsed = parseTag(normalized);
    if (parsed.language.empty()) {
      return candidates;
    }

    std::string script = parsed.script;
    if (script.empty() && !parsed.region.empty()) {
      script = inferredChineseScript(parsed.language, parsed.region);
      appendUnique(candidates, withInsertedScript(parsed, script));
    }

    appendUnique(candidates, normalized);

    if (!parsed.script.empty() && !parsed.region.empty()) {
      appendUnique(candidates, withoutScript(parsed));
    }

    appendUnique(candidates, scriptOnly(parsed, script));
    appendUnique(candidates, parsed.language);
    return candidates;
  }

} // namespace i18n::detail
