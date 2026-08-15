#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wallpaper {

  class ShuffleState {
  public:
    void setStatePath(std::filesystem::path path);

    [[nodiscard]] std::string pick(
        std::string_view scope, std::string_view source, const std::vector<std::string>& candidates,
        std::string_view currentPath, float randomUnit
    );

  private:
    void load();
    void save() const;

    std::filesystem::path m_statePath;
    std::unordered_map<std::string, std::unordered_map<std::string, std::unordered_set<std::string>>> m_seenByScope;
  };

} // namespace wallpaper
