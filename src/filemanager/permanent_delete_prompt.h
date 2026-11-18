#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace filemanager {

  inline std::string permanentDeletePrompt(const std::vector<std::string>& paths, std::size_t selectedCount) {
    if (selectedCount == 1 && paths.size() == 1) {
      const auto name = std::filesystem::path(paths.front()).filename().string();
      return "Permanently delete \"" + (name.empty() ? std::string("item") : name)
          + "\"? This cannot be undone.";
    }
    return "Permanently delete " + std::to_string(selectedCount) + " selected items? This cannot be undone.";
  }

} // namespace filemanager
