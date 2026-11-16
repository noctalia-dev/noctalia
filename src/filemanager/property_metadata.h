#pragma once

#include <filesystem>
#include <gio/gio.h>
#include <string>

namespace filemanager {
  inline std::string propertyTypeDescription(GFileInfo* info) {
    if (g_file_info_get_is_symlink(info))
      return "Symbolic link";
    const auto type = g_file_info_get_file_type(info);
    if (type == G_FILE_TYPE_DIRECTORY)
      return "Folder";
    if (const auto* content = g_file_info_get_content_type(info)) {
      char* description = g_content_type_get_description(content);
      std::string result = description ? description : "";
      g_free(description);
      if (!result.empty())
        return result;
    }
    return type == G_FILE_TYPE_REGULAR ? "File" : "Unknown item";
  }
  inline bool propertyMountTarget(std::filesystem::path path, std::filesystem::path mount) {
    const auto normalize = [](std::filesystem::path value) {
      value = value.lexically_normal();
      while (value != value.root_path() && value.filename().empty())
        value = value.parent_path();
      return value;
    };
    return path.is_absolute() && mount.is_absolute() && normalize(path) == normalize(mount);
  }
  inline std::string propertyFilenameSuffix(const std::filesystem::path& path, bool directory) {
    return directory ? std::string{} : path.extension().string();
  }
} // namespace filemanager
