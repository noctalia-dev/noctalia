#pragma once

#include <algorithm>
#include <filesystem>
#include <gio/gio.h>
#include <string>
#include <string_view>

namespace filemanager {
  inline std::string propertyFilesystemTypeDescription(std::string_view type, std::string_view options = {}) {
    if (type == "nfs4")
      return "NFSv4";
    if (type != "nfs")
      return std::string(type);
    while (!options.empty()) {
      const auto comma = options.find(',');
      const auto option = options.substr(0, comma);
      for (const auto key : {std::string_view("vers="), std::string_view("nfsvers=")}) {
        if (option.starts_with(key)) {
          const auto version = option.substr(key.size());
          const auto minor = version.starts_with("4.") ? version.substr(2) : std::string_view{};
          if (version == "4"
              || (!minor.empty()
                  && std::all_of(minor.begin(), minor.end(), [](unsigned char ch) { return g_ascii_isdigit(ch); })))
            return "NFSv4";
        }
      }
      if (comma == std::string_view::npos)
        break;
      options.remove_prefix(comma + 1);
    }
    return "NFS";
  }

  enum class PropertyApplyStep { None, Rename, Metadata };
  inline PropertyApplyStep
  propertyApplyStep(const std::string& originalName, const std::string& draft, bool metadataPending) {
    return originalName != draft ? PropertyApplyStep::Rename
        : metadataPending        ? PropertyApplyStep::Metadata
                                 : PropertyApplyStep::None;
  }

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
