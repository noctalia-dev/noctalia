#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace i18n {

  // Transparent hash/eq so Catalog::find() can take string_view without
  // allocating a temporary std::string.
  struct StringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
    std::size_t operator()(const std::string& s) const noexcept { return std::hash<std::string_view>{}(s); }
    std::size_t operator()(const char* s) const noexcept { return std::hash<std::string_view>{}(s); }
  };

  struct StringEq {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
  };

  using Catalog = std::unordered_map<std::string, std::string, StringHash, StringEq>;

  struct LanguageOption {
    std::string_view code;
    std::string_view displayName;
  };

  inline constexpr std::array<LanguageOption, 22> kSupportedLanguages = {{
      {"be", "Беларуская"}, {"be-Latn", "Biełaruskaja (Łacinka)"},
      {"ca", "Català"},     {"cs", "Čeština"},
      {"de", "Deutsch"},    {"en", "English"},
      {"es", "Español"},    {"fr", "Français"},
      {"gl-ES", "Galego"},  {"hu", "Magyar"},
      {"it", "Italiano"},   {"ku", "Kurdî"},
      {"nl", "Nederlands"}, {"nn", "Norsk nynorsk"},
      {"pl", "Polski"},     {"pt-BR", "Português (Brasil)"},
      {"ru", "Русский"},    {"sv", "Svenska"},
      {"tr", "Türkçe"},     {"uk-UA", "Українська"},
      {"vi", "Tiếng Việt"}, {"zh-Hans", "简体中文"},
  }};

  // Loads translation catalogs and resolves dotted keys against them.
  // English is always loaded as a fallback alongside the active language.
  class Service {
  public:
    static Service& instance();

    // Loads the active catalog. Pass an empty view to auto-detect from the
    // standard locale environment ($LC_ALL, $LC_MESSAGES, $LANG). Always
    // loads English as a fallback first; if the detected language is also
    // English, both slots end up pointing at the same catalog contents.
    void init(std::string_view preferredLang = {});

    void setLanguage(std::string_view lang);
    [[nodiscard]] std::string_view language() const noexcept { return m_language; }

    // Returns a view into the active or fallback catalog, or {} if the key
    // exists in neither.
    [[nodiscard]] std::string_view lookup(std::string_view dottedKey) const;

  private:
    bool loadCatalog(std::string_view lang, Catalog& out) const;

    Catalog m_active;
    Catalog m_fallback;
    std::string m_language;
  };

} // namespace i18n
