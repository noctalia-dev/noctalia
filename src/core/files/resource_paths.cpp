#include "core/files/resource_paths.h"

#include "core/log.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <optional>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace paths {

  namespace {

    constexpr Logger kLog("paths");
    bool s_usingFilesAssets = false;

    std::filesystem::path installedAssetsRoot() {
      const std::filesystem::path datadir(NYTHERA_INSTALL_DATADIR);
      if (datadir.is_absolute()) {
        return datadir / "nythera" / "assets";
      }
      return std::filesystem::path(NYTHERA_INSTALL_PREFIX) / datadir / "nythera" / "assets";
    }

    bool isAssetRoot(const std::filesystem::path& root) {
      if (root.empty()) {
        return false;
      }

      std::error_code ec;
      return std::filesystem::exists(root / "emoji.json", ec)
          && std::filesystem::exists(root / "fonts" / "nythera-tabler.ttf", ec)
          && std::filesystem::exists(root / "templates" / "builtin.toml", ec)
          && std::filesystem::exists(root / "translations" / "en.json", ec);
    }

    std::optional<std::filesystem::path> executablePath() {
      std::array<char, 4096> buffer{};
      const ssize_t count = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
      if (count <= 0 || static_cast<std::size_t>(count) >= buffer.size() - 1) {
        return std::nullopt;
      }

      buffer[static_cast<std::size_t>(count)] = '\0';
      return std::filesystem::path(buffer.data());
    }

    bool isFilesAssetRoot(const std::filesystem::path& root) {
      std::error_code ec;
      // The Tabler font and catalogue are optional. Keep a single root even if either is absent.
      return std::filesystem::is_regular_file(root / "fonts" / "NytheraDisplay-Regular.ttf", ec)
          && std::filesystem::is_regular_file(root / "translations" / "en.json", ec);
    }

    std::filesystem::path installedFilesAssetsRoot() {
      const std::filesystem::path datadir(NYTHERA_INSTALL_DATADIR);
      if (datadir.is_absolute()) {
        return datadir / "nythera" / "files-assets";
      }
      return std::filesystem::path(NYTHERA_INSTALL_PREFIX) / datadir / "nythera" / "files-assets";
    }

    std::filesystem::path resolveFilesAssetsRoot() {
      if (const char* env = std::getenv("NYTHERA_ASSETS_DIR"); env != nullptr && env[0] != '\0') {
        const std::filesystem::path overridePath(env);
        if (isFilesAssetRoot(overridePath)) {
          kLog.debug("using Files assets from {}", overridePath.string());
          return overridePath;
        }
        kLog.warn("NYTHERA_ASSETS_DIR is not a valid Files asset bundle: {}", overridePath.string());
      }

      const auto executable = executablePath();
      if (!executable) {
        kLog.error("could not locate the Files executable to resolve its asset bundle");
        return {};
      }
      const auto exeDir = executable->parent_path();
      const auto bundle = exeDir / "files-assets";
      for (const auto& candidate : {bundle, exeDir / "assets", exeDir.parent_path() / "assets"}) {
        if (isFilesAssetRoot(candidate)) {
          kLog.debug("using Files assets from {}", candidate.string());
          return candidate;
        }
      }
      const auto installed = installedFilesAssetsRoot();
      if (isFilesAssetRoot(installed)) {
        kLog.debug("using installed Files assets from {}", installed.string());
        return installed;
      }
      kLog.warn("could not locate a valid Files asset bundle; defaulting to {}", installed.string());
      return installed;
    }

    void appendUnique(std::vector<std::filesystem::path>& candidates, const std::filesystem::path& candidate) {
      if (candidate.empty()) {
        return;
      }
      if (!std::ranges::contains(candidates, candidate)) {
        candidates.push_back(candidate);
      }
    }

    std::vector<std::filesystem::path> assetCandidates() {
      std::vector<std::filesystem::path> candidates;

      if (const char* env = std::getenv("NYTHERA_ASSETS_DIR"); env != nullptr && env[0] != '\0') {
        const std::filesystem::path overridePath(env);
        if (isAssetRoot(overridePath)) {
          candidates.push_back(overridePath);
          return candidates;
        }
        kLog.warn("NYTHERA_ASSETS_DIR is not a valid asset bundle: {}", overridePath.string());
      }

      if (auto exe = executablePath()) {
        const std::filesystem::path exeDir = exe->parent_path();
        appendUnique(candidates, exeDir / "assets");
        appendUnique(candidates, exeDir.parent_path() / "assets");

        const std::filesystem::path datadir(NYTHERA_INSTALL_DATADIR);
        if (!datadir.empty() && !datadir.is_absolute()) {
          appendUnique(candidates, exeDir.parent_path() / datadir / "nythera" / "assets");
        }
        appendUnique(candidates, exeDir.parent_path() / "share" / "nythera" / "assets");
      }

      appendUnique(candidates, installedAssetsRoot());
      return candidates;
    }

    std::filesystem::path resolveAssetsRoot() {
      for (const auto& candidate : assetCandidates()) {
        if (isAssetRoot(candidate)) {
          kLog.debug("using assets from {}", candidate.string());
          return candidate;
        }
      }

      const std::filesystem::path fallback = installedAssetsRoot();
      kLog.warn("could not locate a valid asset bundle; defaulting to {}", fallback.string());
      return fallback;
    }

  } // namespace

  void selectFilesAssets() noexcept { s_usingFilesAssets = true; }

  bool usingFilesAssets() noexcept { return s_usingFilesAssets; }

  const std::filesystem::path& assetsRoot() {
    static const std::filesystem::path s_root = s_usingFilesAssets ? resolveFilesAssetsRoot() : resolveAssetsRoot();
    return s_root;
  }

  std::filesystem::path assetPath(std::string_view relativePath) {
    return assetsRoot() / std::filesystem::path(std::string(relativePath));
  }

} // namespace paths
