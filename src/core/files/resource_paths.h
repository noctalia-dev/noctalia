#pragma once

#include <filesystem>
#include <string_view>

namespace paths {

  const std::filesystem::path& assetsRoot();
  std::filesystem::path assetPath(std::string_view relativePath);

} // namespace paths
