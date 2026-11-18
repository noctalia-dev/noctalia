#pragma once

#include <filesystem>
#include <glib.h>
#include <stdexcept>
#include <string>
#include <string_view>

namespace filemanager {

inline std::string startupPath(std::string_view argument) {
  std::string path(argument.empty() ? g_get_home_dir() : argument);
  if (path.starts_with("file:")) {
    gchar* hostname = nullptr;
    GError* error = nullptr;
    gchar* decoded = g_filename_from_uri(path.c_str(), &hostname, &error);
    const bool local = !hostname || !*hostname || g_ascii_strcasecmp(hostname, "localhost") == 0;
    g_free(hostname);
    if (!decoded || !local) {
      std::string message = error ? error->message : "Only local file URIs are supported";
      g_clear_error(&error);
      g_free(decoded);
      throw std::runtime_error(message);
    }
    path = decoded;
    g_free(decoded);
  }
  std::error_code error;
  if (!std::filesystem::is_directory(path, error))
    throw std::runtime_error("Not an accessible local directory: " + path);
  return std::filesystem::absolute(path).string();
}

} // namespace filemanager
