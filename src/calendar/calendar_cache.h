#pragma once

#include <filesystem>
#include <string_view>

namespace calendar::cache {
  [[nodiscard]] bool secureExisting(const std::filesystem::path& path);
  [[nodiscard]] bool write(const std::filesystem::path& path, std::string_view content);
} // namespace calendar::cache
