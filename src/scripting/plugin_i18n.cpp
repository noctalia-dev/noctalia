#include "scripting/plugin_i18n.h"

#include "core/log.h"
#include "i18n/i18n_service.h"

#include <fstream>
#include <json.hpp>

namespace scripting {

  namespace {
    constexpr Logger kLog("plugin-i18n");

    void flattenTranslations(
        const nlohmann::json& node, const std::string& prefix, std::unordered_map<std::string, std::string>& out
    ) {
      if (node.is_object()) {
        for (const auto& [key, value] : node.items()) {
          flattenTranslations(value, prefix.empty() ? key : prefix + "." + key, out);
        }
      } else if (node.is_string()) {
        out[prefix] = node.get<std::string>();
      }
    }

    void mergeTranslationFile(const std::filesystem::path& path, std::unordered_map<std::string, std::string>& out) {
      std::ifstream file(path);
      if (!file) {
        return;
      }
      try {
        flattenTranslations(nlohmann::json::parse(file), {}, out);
      } catch (const nlohmann::json::exception&) {
        kLog.warn("failed to parse plugin translations: {}", path.string());
      }
    }
  } // namespace

  void PluginTranslationCatalog::load(const std::filesystem::path& pluginDir) {
    m_values.clear();
    if (pluginDir.empty()) {
      return;
    }

    const std::filesystem::path dir = pluginDir / "translations";
    mergeTranslationFile(dir / "en.json", m_values);
    if (const std::string_view lang = i18n::Service::instance().language(); !lang.empty() && lang != "en") {
      mergeTranslationFile(dir / (std::string(lang) + ".json"), m_values);
    }
  }

  bool PluginTranslationCatalog::has(std::string_view key) const { return m_values.contains(std::string(key)); }

  std::string PluginTranslationCatalog::translate(
      std::string_view key, const std::unordered_map<std::string, std::string>& subst
  ) const {
    const auto it = m_values.find(std::string(key));
    if (it == m_values.end()) {
      kLog.warn("plugin translation key '{}' not found", key);
      return std::string(key);
    }
    const std::string& tmpl = it->second;
    if (subst.empty() || !tmpl.contains('{')) {
      return tmpl;
    }

    std::string out;
    out.reserve(tmpl.size());
    for (std::size_t i = 0; i < tmpl.size();) {
      if (tmpl[i] == '{') {
        const std::size_t end = tmpl.find('}', i + 1);
        if (end != std::string::npos) {
          const std::string name = tmpl.substr(i + 1, end - i - 1);
          if (const auto found = subst.find(name); found != subst.end()) {
            out += found->second;
          } else {
            out.append(tmpl, i, end - i + 1);
          }
          i = end + 1;
          continue;
        }
      }
      out.push_back(tmpl[i]);
      ++i;
    }
    return out;
  }

} // namespace scripting
