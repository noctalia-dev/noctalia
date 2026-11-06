#pragma once

#include <filesystem>
#include <string_view>

namespace paths {

  struct AssetRoots {
    std::filesystem::path application;
    std::filesystem::path library;
  };

  // Set once on the startup thread, before any asset lookup or worker starts. The application
  // resolves its environment/install paths and validates its own required assets. Both roots must
  // be directories; the library root must contain its glyph font and metadata. Invalid roots throw
  // without searching elsewhere. Relative roots are anchored to the startup working directory.
  void setAssetRoots(AssetRoots roots);

  // Uses the explicit application root when configured. Otherwise the shell's existing discovery
  // uses <envPrefix>_ASSETS_DIR, the executable's location, and <datadir>/<name>/assets.
  const std::filesystem::path& assetsRoot();
  std::filesystem::path assetPath(std::string_view relativePath);

  // The library's own assets (lib/assets: the glyph font and its metadata). Installed applications
  // copy them into their asset directory; a build-tree run finds lib/assets beside the build dir.
  // An explicit library root bypasses discovery, including the application asset requirements.
  const std::filesystem::path& libraryAssetsRoot();
  std::filesystem::path libraryAssetPath(std::string_view relativePath);

} // namespace paths
