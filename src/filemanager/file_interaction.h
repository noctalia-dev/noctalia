#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace filemanager {
  inline bool validFileName(const std::string& name) {
    return !name.empty()
        && name != "."
        && name != ".."
        && name.find('/') == std::string::npos
        && name.find('\0') == std::string::npos;
  }

  // Resolve only explicitly selected paths and the chosen drop target. Discovery
  // and recursive copy retain their existing no-follow policies.
  inline std::optional<std::filesystem::path>
  checkedDropTarget(const std::vector<std::string>& sources, const std::filesystem::path& destination) {
    std::error_code error;
    auto target = std::filesystem::canonical(destination, error);
    if (sources.empty() || error || !std::filesystem::is_directory(target, error))
      return std::nullopt;
    for (const auto& source : sources) {
      // Transfers operate on the directory entry, including dangling links.
      // Resolve its parent for same-folder checks, and follow only real folders
      // when guarding against recursive copies into their descendants.
      const auto sourcePath = std::filesystem::path(source).lexically_normal();
      const auto status = std::filesystem::symlink_status(sourcePath, error);
      if (error || !std::filesystem::exists(status))
        return std::nullopt;
      const auto parent = std::filesystem::canonical(sourcePath.parent_path(), error);
      if (error || parent == target)
        return std::nullopt;
      if (std::filesystem::is_directory(status)) {
        const auto path = std::filesystem::canonical(sourcePath, error);
        if (error)
          return std::nullopt;
        const auto relative = target.lexically_relative(path);
        if (target == path || (!relative.empty() && *relative.begin() != ".."))
          return std::nullopt;
      }
    }
    return target;
  }

  inline bool copyDropByDefault(const std::vector<std::string>& sources, const std::filesystem::path& destination) {
    struct stat target{};
    if (stat(destination.c_str(), &target) != 0)
      return true;
    for (const auto& source : sources) {
      struct stat item{};
      if (lstat(source.c_str(), &item) != 0 || item.st_dev != target.st_dev)
        return true;
    }
    return false;
  }
} // namespace filemanager
