#include "i18n/i18n_service.h"

#include "core/files/resource_paths.h"
#include "core/log.h"
#include "i18n/language_tag.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace i18n {

  namespace {

    constexpr Logger kLog("i18n");

    void flatten(const nlohmann::json& node, const std::string& prefix, Catalog& out) {
      for (auto it = node.begin(); it != node.end(); ++it) {
        std::string path = prefix.empty() ? it.key() : prefix + "." + it.key();
        if (it->is_object()) {
          flatten(*it, path, out);
        } else if (it->is_string()) {
          out.insert_or_assign(std::move(path), it->get<std::string>());
        }
      }
    }

    // Reads $LC_ALL / $LC_MESSAGES / $LANG. Returns empty string if unset.
    std::string detectSystemLanguage() {
      for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* value = std::getenv(var);
        if (value != nullptr && value[0] != '\0') {
          return detail::normalizeLanguageTag(value);
        }
      }
      return {};
    }

  } // namespace

  Service& Service::instance() {
    static Service s_instance;
    return s_instance;
  }

  bool Service::loadCatalog(std::string_view lang, Catalog& out) const {
    const std::filesystem::path path = paths::assetPath("translations/" + std::string(lang) + ".json");
    std::ifstream file(path);
    if (!file.is_open()) {
      return false;
    }
    try {
      auto json = nlohmann::json::parse(file);
      if (!json.is_object()) {
        kLog.warn("catalog {} is not a JSON object", path.string());
        return false;
      }
      Catalog fresh;
      flatten(json, {}, fresh);
      out = std::move(fresh);
      return true;
    } catch (const std::exception& e) {
      kLog.error("failed to parse {}: {}", path.string(), e.what());
      return false;
    }
  }

  void Service::init(std::string_view preferredLang) {
    std::string candidate;
    if (!preferredLang.empty()) {
      candidate = detail::normalizeLanguageTag(preferredLang);
    } else {
      candidate = detectSystemLanguage();
    }

    // English fallback is always loaded first so lookup() can fall back to it
    // even if the active catalog is English itself.
    if (!loadCatalog("en", m_fallback)) {
      kLog.warn("could not load English fallback catalog");
    }

    if (candidate.empty() || candidate == "en") {
      m_active = m_fallback;
      m_language = "en";
      kLog.info("language: en");
      return;
    }

    for (const std::string& lang : detail::catalogLanguageCandidates(candidate)) {
      if (loadCatalog(lang, m_active)) {
        m_language = lang;
        if (lang == candidate) {
          kLog.info("language: {}", m_language);
        } else {
          kLog.info("language: {} (from {})", m_language, candidate);
        }
        return;
      }
    }

    kLog.warn("no catalog for '{}', falling back to English", candidate);
    m_active = m_fallback;
    m_language = "en";
  }

  void Service::setLanguage(std::string_view lang) {
    if (lang == m_language) {
      return;
    }
    init(lang);
  }

  std::string_view Service::lookup(std::string_view dottedKey) const {
    if (auto it = m_active.find(dottedKey); it != m_active.end()) {
      return it->second;
    }
    if (auto it = m_fallback.find(dottedKey); it != m_fallback.end()) {
      return it->second;
    }
    return {};
  }

} // namespace i18n
