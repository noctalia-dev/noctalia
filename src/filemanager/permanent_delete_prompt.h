#pragma once

#include "core/files/display_text.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace filemanager {

  inline std::string permanentDeletePrompt(const std::vector<std::string>& paths, std::size_t selectedCount) {
    if (selectedCount == 1 && paths.size() == 1) {
      const auto name = std::filesystem::path(paths.front()).filename().string();
      const auto display = name.empty() ? std::string("item") : sanitizeFilenameForDisplay(name);
      return "Permanently delete \"" + display + "\"? This cannot be undone.";
    }
    return "Permanently delete " + std::to_string(selectedCount) + " selected items? This cannot be undone.";
  }

} // namespace filemanager
