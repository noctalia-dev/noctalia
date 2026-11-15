#pragma once

#include <filesystem>
#include <gio/gio.h>
#include <string>

namespace filemanager {
  // udev maintains these aliases from filesystem labels, including volumes GIO
  // deliberately omits from its user-visible volume monitor (notably host root).
  inline std::string
  filesystemLabel(const std::filesystem::path& device, const std::filesystem::path& labels = "/dev/disk/by-label") {
    std::error_code error;
    const auto target = std::filesystem::canonical(device, error);
    if (error)
      return {};
    std::filesystem::directory_iterator entries(labels, error);
    for (auto end = std::filesystem::directory_iterator{}; !error && entries != end; entries.increment(error)) {
      const auto candidate = std::filesystem::canonical(entries->path(), error);
      if (error) {
        error.clear();
        continue;
      }
      if (candidate != target)
        continue;
      const auto encoded = entries->path().filename().string();
      std::string label;
      for (std::size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '\\'
            && i + 3 < encoded.size()
            && encoded[i + 1] == 'x'
            && g_ascii_isxdigit(encoded[i + 2])
            && g_ascii_isxdigit(encoded[i + 3])) {
          label += static_cast<char>(g_ascii_xdigit_value(encoded[i + 2]) * 16 + g_ascii_xdigit_value(encoded[i + 3]));
          i += 3;
        } else
          label += encoded[i];
      }
      return label;
    }
    return {};
  }
} // namespace filemanager
