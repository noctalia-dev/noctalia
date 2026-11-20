#pragma once

#include <filesystem>
#include <gio/gio.h>
#include <string>
#include <vector>

struct FileArtwork {
  std::vector<std::string> names;
  bool symlink = false;
  bool broken = false;
};

// GIO backends may omit a requested attribute. Avoid the accessor's critical
// warning and treat an unavailable type as unknown.
inline GFileType gFileInfoTypeOrUnknown(GFileInfo* info) {
  if (info == nullptr || !g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_TYPE))
    return G_FILE_TYPE_UNKNOWN;
  return g_file_info_get_file_type(info);
}

inline bool isBoundedDirectoryMetadataFile(GFileInfo* info) {
  return gFileInfoTypeOrUnknown(info) == G_FILE_TYPE_REGULAR
      && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE)
      && g_file_info_get_size(info) <= 65536;
}

// Query without following first so the displayed identity remains the link.
// Following is only used to describe its target, never for file operations.
inline std::string queryFileTypeDescription(const std::filesystem::path& path) {
  GFile* file =
      path.string().starts_with("trash:/") ? g_file_new_for_uri(path.c_str()) : g_file_new_for_path(path.c_str());
  constexpr const char* attributes =
      "standard::type,standard::is-symlink,standard::symlink-target,standard::content-type";
  GFileInfo* info = g_file_query_info(file, attributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr);
  const bool symlink = info != nullptr
      && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK)
      && g_file_info_get_is_symlink(info);
  std::string target;
  if (symlink) {
    if (const char* value = g_file_info_get_symlink_target(info); value != nullptr && *value != '\0') {
      const std::filesystem::path raw(value);
      // Lexical resolution retains useful target spelling without requiring the
      // destination to exist or accidentally following a symlink loop.
      target = (raw.is_absolute() ? raw : path.parent_path() / raw).lexically_normal().string();
    }
    g_clear_object(&info);
    info = g_file_query_info(file, attributes, G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
  }
  const bool broken = symlink && (info == nullptr || gFileInfoTypeOrUnknown(info) == G_FILE_TYPE_SYMBOLIC_LINK);
  std::string description = broken ? "Broken symbolic link" : "File";
  if (info != nullptr && !broken) {
    const char* contentType = g_file_info_get_content_type(info);
    gchar* typeName = contentType == nullptr ? nullptr : g_content_type_get_description(contentType);
    description = gFileInfoTypeOrUnknown(info) == G_FILE_TYPE_DIRECTORY ? "Folder"
        : typeName != nullptr                                           ? typeName
                                                                        : "File";
    g_free(typeName);
  }
  if (symlink)
    description += target.empty() ? " (symbolic link)" : " (link to " + target + ")";
  g_clear_object(&info);
  g_object_unref(file);
  return description;
}

inline FileArtwork guessFileArtwork(const std::filesystem::path& path, bool directory) {
  FileArtwork result;
  if (!directory && !path.empty()) {
    gchar* contentType = g_content_type_guess(path.filename().c_str(), nullptr, 0, nullptr);
    GIcon* icon = contentType == nullptr ? nullptr : g_content_type_get_icon(contentType);
    if (icon != nullptr && G_IS_THEMED_ICON(icon)) {
      const gchar* const* names = g_themed_icon_get_names(G_THEMED_ICON(icon));
      for (auto name = names; name != nullptr && *name != nullptr; ++name)
        result.names.emplace_back(*name);
    }
    g_clear_object(&icon);
    g_free(contentType);
  }
  result.names.push_back(directory ? "folder" : "text-x-generic");
  return result;
}

inline std::string
guessFileTypeDescription(const std::filesystem::path& path, bool directory, const std::string& contentType = {}) {
  if (directory)
    return "Folder";
  if (!contentType.empty()) {
    gchar* typeName = g_content_type_get_description(contentType.c_str());
    std::string description = typeName != nullptr ? typeName : "File";
    g_free(typeName);
    return description;
  }
  if (!path.empty()) {
    gchar* guessed = g_content_type_guess(path.filename().c_str(), nullptr, 0, nullptr);
    gchar* typeName = guessed == nullptr ? nullptr : g_content_type_get_description(guessed);
    std::string description = typeName != nullptr ? typeName : "File";
    g_free(typeName);
    g_free(guessed);
    return description;
  }
  return "File";
}

inline FileArtwork
artworkFromFileInfo(GFileInfo* info, const std::filesystem::path& path, bool directory, bool symlink, bool broken) {
  FileArtwork result;
  result.symlink = symlink;
  result.broken = broken;
  GIcon* icon = info == nullptr || result.broken ? nullptr : g_file_info_get_icon(info);
  if (icon != nullptr && G_IS_EMBLEMED_ICON(icon))
    icon = g_emblemed_icon_get_icon(G_EMBLEMED_ICON(icon));
  gchar* contentType = nullptr;
  GIcon* fallback = nullptr;
  if (icon == nullptr && !result.broken) {
    contentType = g_content_type_guess(path.filename().c_str(), nullptr, 0, nullptr);
    fallback = contentType == nullptr ? nullptr : g_content_type_get_icon(contentType);
    icon = fallback;
  }
  if (icon != nullptr && G_IS_THEMED_ICON(icon)) {
    const gchar* const* names = g_themed_icon_get_names(G_THEMED_ICON(icon));
    for (auto name = names; name != nullptr && *name != nullptr; ++name)
      result.names.emplace_back(*name);
  }
  if (result.broken) {
    result.names = {"text-x-generic", "unknown"};
  } else {
    result.names.push_back(directory ? "folder" : "text-x-generic");
  }
  g_clear_object(&fallback);
  g_free(contentType);
  return result;
}

inline FileArtwork queryFileArtwork(const std::filesystem::path& path, bool directory) {
  GFile* file =
      path.string().starts_with("trash:/") ? g_file_new_for_uri(path.c_str()) : g_file_new_for_path(path.c_str());
  constexpr const char* attributes = "standard::icon,standard::type,standard::is-symlink,standard::content-type";
  GFileInfo* info = g_file_query_info(file, attributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr);
  const bool symlink = info != nullptr
      && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK)
      && g_file_info_get_is_symlink(info);
  bool broken = false;
  if (symlink) {
    g_clear_object(&info);
    info = g_file_query_info(file, attributes, G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
    broken = info == nullptr || gFileInfoTypeOrUnknown(info) == G_FILE_TYPE_SYMBOLIC_LINK;
    directory = !broken && gFileInfoTypeOrUnknown(info) == G_FILE_TYPE_DIRECTORY;
  }
  FileArtwork result = artworkFromFileInfo(info, path, directory, symlink, broken);
  g_clear_object(&info);
  g_object_unref(file);
  return result;
}
