#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <vector>

namespace filemanager {
  inline std::string expandHomePath(std::string_view path, std::string_view home) {
    if (path == "~" || path == "~/")
      return std::string(home);
    if (!path.starts_with("~/"))
      return std::string(path);
    std::string expanded(home);
    expanded.append(path.substr(home.ends_with('/') ? 2 : 1));
    return expanded;
  }

  inline constexpr bool locationEditCancelIsCurrent(bool editing, std::uint64_t current, std::uint64_t captured) {
    return editing && current == captured;
  }

  inline constexpr bool pointerOutsideRect(float x, float y, float left, float top, float width, float height) {
    return x < left || y < top || x >= left + width || y >= top + height;
  }

  inline bool isHiddenFileName(const std::string& name) { return !name.empty() && name.front() == '.'; }

  inline constexpr float fileEntryIconNameAlpha(bool hidden, bool selected, bool hovered, bool disabled) {
    return disabled ? 0.55F : (hidden && !selected && !hovered ? 0.78F : 1.0F);
  }

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
