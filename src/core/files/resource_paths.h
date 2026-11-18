#pragma once

#include <filesystem>
#include <string_view>

namespace paths {

  // Select before the standalone process first accesses any assets.
  void selectFilesAssets() noexcept;
  [[nodiscard]] bool usingFilesAssets() noexcept;

  [[nodiscard]] const std::filesystem::path& assetsRoot();
  [[nodiscard]] std::filesystem::path assetPath(std::string_view relativePath);

} // namespace paths
