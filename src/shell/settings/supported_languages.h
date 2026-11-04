#pragma once

#include <array>
#include <string_view>

namespace settings {

  struct LanguageOption {
    std::string_view code;
    std::string_view displayName;
  };

  // Languages offered by the shell's language setting. Every entry has a
  // catalog in assets/translations and every catalog has an entry here.
  inline constexpr std::array<LanguageOption, 29> kSupportedLanguages = {{
      {"ar", "العربية"},
      {"be", "Беларуская"},
      {"be-Latn", "Biełaruskaja (Łacinka)"},
      {"ca", "Català"},
      {"cs", "Čeština"},
      {"de", "Deutsch"},
      {"en", "English"},
      {"en-GB", "English (United Kingdom)"},
      {"es", "Español"},
      {"fr", "Français"},
      {"gl-ES", "Galego"},
      {"he", "עברית"},
      {"hu", "Magyar"},
      {"it", "Italiano"},
      {"ja", "日本語"},
      {"ko", "한국어"},
      {"ku", "Kurdî"},
      {"nl", "Nederlands"},
      {"nn", "Norsk nynorsk"},
      {"pl", "Polski"},
      {"pt-BR", "Português (Brasil)"},
      {"ro", "Română"},
      {"ru", "Русский"},
      {"sv", "Svenska"},
      {"tr", "Türkçe"},
      {"uk-UA", "Українська"},
      {"vi", "Tiếng Việt"},
      {"zh-Hans", "简体中文"},
      {"zh-Hant", "繁體中文"},
  }};

} // namespace settings
