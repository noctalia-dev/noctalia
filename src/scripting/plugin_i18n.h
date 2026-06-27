#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace scripting {

  class PluginTranslationCatalog {
  public:
    void load(const std::filesystem::path& pluginDir);

    [[nodiscard]] bool has(std::string_view key) const;
    [[nodiscard]] std::string
    translate(std::string_view key, const std::unordered_map<std::string, std::string>& subst = {}) const;

  private:
    std::unordered_map<std::string, std::string> m_values;
  };

} // namespace scripting
