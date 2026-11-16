#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <gio/gio.h>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace filemanager {
  inline constexpr std::size_t directoryMetadataMaxBytes = 65536;

  // Return the themed icon from a bounded .directory file before the existing
  // GIO/theme fallback names. Invalid metadata leaves fallbackNames untouched.
  inline std::optional<std::string> directoryMetadataIcon(const char* data, std::size_t length) {
    if (!data || length == 0 || length > directoryMetadataMaxBytes || std::memchr(data, '\0', length))
      return std::nullopt;

    GKeyFile* keyFile = g_key_file_new();
    const bool loaded = g_key_file_load_from_data(keyFile, data, length, G_KEY_FILE_NONE, nullptr);
    gchar* type = loaded ? g_key_file_get_string(keyFile, "Desktop Entry", "Type", nullptr) : nullptr;
    gchar* icon = loaded ? g_key_file_get_string(keyFile, "Desktop Entry", "Icon", nullptr) : nullptr;
    const bool validType = type && g_strcmp0(type, "Directory") == 0;
    const bool safeName = icon
        && *icon
        && std::strlen(icon) <= 256
        && std::strchr(icon, '/') == nullptr
        && std::strchr(icon, '\\') == nullptr
        && std::strcmp(icon, ".") != 0
        && std::strcmp(icon, "..") != 0
        && std::strchr(icon, ':') == nullptr;
    std::optional<std::string> result;
    if (validType && safeName)
      result = icon;
    g_free(type);
    g_free(icon);
    g_key_file_unref(keyFile);
    return result;
  }

  inline std::vector<std::string>
  directoryMetadataIconNames(const char* data, std::size_t length, std::vector<std::string> fallbackNames) {
    const auto icon = directoryMetadataIcon(data, length);
    if (icon) {
      const auto found = std::ranges::find(fallbackNames, *icon);
      if (found == fallbackNames.end())
        fallbackNames.insert(fallbackNames.begin(), *icon);
      else if (found != fallbackNames.begin())
        std::rotate(fallbackNames.begin(), found, std::next(found));
    }
    return fallbackNames;
  }

  inline std::vector<std::string>
  prioritizeDirectoryMetadataIcon(const std::string& icon, std::vector<std::string> fallbackNames) {
    if (icon.empty())
      return fallbackNames;
    const auto found = std::ranges::find(fallbackNames, icon);
    if (found == fallbackNames.end())
      fallbackNames.insert(fallbackNames.begin(), icon);
    else if (found != fallbackNames.begin())
      std::rotate(fallbackNames.begin(), found, std::next(found));
    return fallbackNames;
  }
} // namespace filemanager
