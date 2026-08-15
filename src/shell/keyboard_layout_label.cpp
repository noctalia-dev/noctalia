#include "shell/keyboard_layout_label.h"

#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <utility>
#include <vector>

namespace {
  constexpr std::string_view kUnknownLabel = "--";
  bool isAsciiAlpha(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); }

  bool isWordBoundary(std::string_view text, std::size_t pos) {
    if (pos >= text.size()) {
      return true;
    }
    return !std::isalnum(static_cast<unsigned char>(text[pos])) && text[pos] != '_';
  }

  bool containsWord(std::string_view haystack, std::string_view needle) {
    if (haystack.empty() || needle.empty()) {
      return false;
    }

    std::size_t pos = haystack.find(needle);
    while (pos != std::string_view::npos) {
      if (isWordBoundary(haystack, pos == 0 ? haystack.size() : pos - 1)
          && isWordBoundary(haystack, pos + needle.size())) {
        return true;
      }
      pos = haystack.find(needle, pos + 1);
    }
    return false;
  }

  bool extractLeadingCode(std::string_view text, std::string& out) {
    std::size_t count = 0;
    while (count < text.size() && count < 3 && isAsciiAlpha(text[count])) {
      ++count;
    }
    if (count < 2 || count > 3) {
      return false;
    }
    if (count < text.size()
        && text[count] != '+'
        && !std::isspace(static_cast<unsigned char>(text[count]))
        && text[count] != '_'
        && text[count] != '-') {
      return false;
    }
    out.assign(text.substr(0, count));
    return true;
  }

  bool extractParenthesizedCode(std::string_view text, std::string& out) {
    const std::size_t open = text.find('(');
    const std::size_t close = text.find(')', open == std::string_view::npos ? 0 : open + 1);
    if (open == std::string_view::npos || close == std::string_view::npos || close <= open + 2) {
      return false;
    }
    std::string_view inner = text.substr(open + 1, close - open - 1);
    if (inner.size() < 2 || inner.size() > 3) {
      return false;
    }
    if (!std::ranges::all_of(inner, [](char ch) { return isAsciiAlpha(ch); })) {
      return false;
    }
    out.assign(inner);
    return true;
  }

  void uppercaseAscii(std::string& text) {
    for (char& ch : text) {
      ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
  }

  const std::vector<std::pair<std::string_view, std::string_view>>& variantMap() {
    static const std::vector<std::pair<std::string_view, std::string_view>> kMap = {
        {"programmer dvorak", "Dvk-P"}, {"colemak", "Colemak"}, {"dvorak", "Dvorak"},
        {"workman", "Workman"},         {"norman", "Norman"},   {"altgr-intl", "Intl"},
        {"international", "Intl"},      {"intl", "Intl"},       {"with dead keys", "Dead"},
        {"phonetic", "Phon"},           {"extended", "Ext"},    {"ergonomic", "Ergo"},
        {"legacy", "Legacy"},           {"pinyin", "Pinyin"},   {"cangjie", "Cangjie"},
        {"romaji", "Romaji"},           {"kana", "Kana"},
    };
    return kMap;
  }

  const std::vector<std::pair<std::string_view, std::string_view>>& languageMap() {
    static const std::vector<std::pair<std::string_view, std::string_view>> kMap = {
        {"english", "us"},
        {"american", "us"},
        {"united states", "us"},
        {"us english", "us"},
        {"british", "gb"},
        {"united kingdom", "gb"},
        {"english (uk)", "gb"},
        {"canadian", "ca"},
        {"canada", "ca"},
        {"canadian english", "ca"},
        {"australian", "au"},
        {"australia", "au"},
        {"swedish", "se"},
        {"svenska", "se"},
        {"sweden", "se"},
        {"norwegian", "no"},
        {"norsk", "no"},
        {"norway", "no"},
        {"danish", "dk"},
        {"dansk", "dk"},
        {"denmark", "dk"},
        {"finnish", "fi"},
        {"suomi", "fi"},
        {"finland", "fi"},
        {"icelandic", "is"},
        {"iceland", "is"},
        {"german", "de"},
        {"deutsch", "de"},
        {"germany", "de"},
        {"austrian", "at"},
        {"austria", "at"},
        {"swiss", "ch"},
        {"switzerland", "ch"},
        {"schweiz", "ch"},
        {"suisse", "ch"},
        {"dutch", "nl"},
        {"nederlands", "nl"},
        {"netherlands", "nl"},
        {"holland", "nl"},
        {"belgian", "be"},
        {"belgium", "be"},
        {"french", "fr"},
        {"francais", "fr"},
        {"france", "fr"},
        {"canadian french", "ca"},
        {"spanish", "es"},
        {"espanol", "es"},
        {"spain", "es"},
        {"castilian", "es"},
        {"italian", "it"},
        {"italiano", "it"},
        {"italy", "it"},
        {"portuguese", "pt"},
        {"portugues", "pt"},
        {"portugal", "pt"},
        {"catalan", "ad"},
        {"andorra", "ad"},
        {"romanian", "ro"},
        {"romania", "ro"},
        {"russian", "ru"},
        {"russia", "ru"},
        {"polish", "pl"},
        {"polski", "pl"},
        {"poland", "pl"},
        {"czech", "cz"},
        {"czech republic", "cz"},
        {"slovak", "sk"},
        {"slovakia", "sk"},
        {"ukraine", "ua"},
        {"ukrainian", "ua"},
        {"bulgarian", "bg"},
        {"bulgaria", "bg"},
        {"serbian", "rs"},
        {"serbia", "rs"},
        {"croatian", "hr"},
        {"croatia", "hr"},
        {"slovenian", "si"},
        {"slovenia", "si"},
        {"bosnian", "ba"},
        {"bosnia", "ba"},
        {"macedonian", "mk"},
        {"macedonia", "mk"},
        {"irish", "ie"},
        {"ireland", "ie"},
        {"welsh", "gb"},
        {"wales", "gb"},
        {"scottish", "gb"},
        {"scotland", "gb"},
        {"estonian", "ee"},
        {"estonia", "ee"},
        {"latvian", "lv"},
        {"latvia", "lv"},
        {"lithuanian", "lt"},
        {"lithuania", "lt"},
        {"hungarian", "hu"},
        {"hungary", "hu"},
        {"greek", "gr"},
        {"greece", "gr"},
        {"albanian", "al"},
        {"albania", "al"},
        {"maltese", "mt"},
        {"malta", "mt"},
        {"turkish", "tr"},
        {"turkey", "tr"},
        {"arabic", "ar"},
        {"arab", "ar"},
        {"hebrew", "il"},
        {"israel", "il"},
        {"brazilian", "br"},
        {"brazilian portuguese", "br"},
        {"brasil", "br"},
        {"brazil", "br"},
        {"japanese", "jp"},
        {"japan", "jp"},
        {"korean", "kr"},
        {"korea", "kr"},
        {"south korea", "kr"},
        {"chinese", "cn"},
        {"china", "cn"},
        {"simplified chinese", "cn"},
        {"traditional chinese", "tw"},
        {"taiwan", "tw"},
        {"thai", "th"},
        {"thailand", "th"},
        {"vietnamese", "vn"},
        {"vietnam", "vn"},
        {"hindi", "in"},
        {"india", "in"},
        {"afrikaans", "za"},
        {"south africa", "za"},
        {"south african", "za"},
    };
    return kMap;
  }

  std::string shortLayoutLabel(const std::string& layoutName) {
    if (layoutName.empty()) {
      return std::string(kUnknownLabel);
    }

    const std::string lower = StringUtils::toLower(layoutName);

    std::string code;
    if (extractLeadingCode(lower, code)) {
      uppercaseAscii(code);
      return code;
    }

    for (const auto& [pattern, display] : variantMap()) {
      if (lower.contains(pattern)) {
        return std::string(display);
      }
    }

    if (extractParenthesizedCode(lower, code)) {
      uppercaseAscii(code);
      return code;
    }

    for (const auto& [lang, mapped] : languageMap()) {
      if (lower.starts_with(lang)) {
        code = std::string(mapped);
        uppercaseAscii(code);
        return code;
      }
    }

    for (const auto& [lang, mapped] : languageMap()) {
      if (containsWord(lower, lang)) {
        code = std::string(mapped);
        uppercaseAscii(code);
        return code;
      }
    }

    if (extractLeadingCode(lower, code)) {
      uppercaseAscii(code);
      return code;
    }

    return std::string(kUnknownLabel);
  }
} // namespace

KeyboardLayoutDisplayMode parseKeyboardLayoutDisplayMode(std::string_view value) {
  return value == "full" ? KeyboardLayoutDisplayMode::Full : KeyboardLayoutDisplayMode::Short;
}

std::string formatKeyboardLayoutLabel(const std::string& layoutName, KeyboardLayoutDisplayMode displayMode) {
  if (layoutName.empty()) {
    return std::string(kUnknownLabel);
  }

  if (displayMode == KeyboardLayoutDisplayMode::Full) {
    return layoutName;
  }
  return shortLayoutLabel(layoutName);
}

std::string resolveKeyboardLayoutLabel(
    const std::string& layoutName, KeyboardLayoutDisplayMode displayMode,
    const std::unordered_map<std::string, std::string>& customLabels
) {
  if (const auto it = customLabels.find(layoutName); it != customLabels.end() && !it->second.empty()) {
    return it->second;
  }
  return formatKeyboardLayoutLabel(layoutName, displayMode);
}
