#pragma once

#include <filesystem>
#include <string_view>

namespace paths {

  // The application's assets (translations and everything the application ships). Resolved from
  // <envPrefix>_ASSETS_DIR, the executable's location, and <datadir>/<name>/assets.
  const std::filesystem::path& assetsRoot();
  std::filesystem::path assetPath(std::string_view relativePath);

  // The library's own assets (lib/assets: the glyph font and its metadata). Installed applications
  // copy them into their asset directory; a build-tree run finds lib/assets beside the build dir.
  const std::filesystem::path& libraryAssetsRoot();
  std::filesystem::path libraryAssetPath(std::string_view relativePath);

} // namespace paths
