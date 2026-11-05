#include "core/files/resource_paths.h"

#include "core/application_identity.h"
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

    std::filesystem::path installedAssetsRoot() {
      const std::filesystem::path datadir(NOCTALIA_UI_INSTALL_DATADIR);
      const std::string& name = applicationIdentity().name;
      if (datadir.is_absolute()) {
        return datadir / name / "assets";
      }
      return std::filesystem::path(NOCTALIA_UI_INSTALL_PREFIX) / datadir / name / "assets";
    }

    bool isAssetRoot(const std::filesystem::path& root) {
      if (root.empty()) {
        return false;
      }

      // The i18n service always loads the English catalog, so every application asset bundle has it.
      std::error_code ec;
      return std::filesystem::exists(root / "translations" / "en.json", ec);
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

      const std::string envName = applicationEnvVar("ASSETS_DIR");
      if (const char* env = std::getenv(envName.c_str()); env != nullptr && env[0] != '\0') {
        const std::filesystem::path overridePath(env);
        if (isAssetRoot(overridePath)) {
          candidates.push_back(overridePath);
          return candidates;
        }
        kLog.warn("{} is not a valid asset bundle: {}", envName, overridePath.string());
      }

      if (auto exe = executablePath()) {
        const std::filesystem::path exeDir = exe->parent_path();
        appendUnique(candidates, exeDir / "assets");
        appendUnique(candidates, exeDir.parent_path() / "assets");

        const std::filesystem::path datadir(NOCTALIA_UI_INSTALL_DATADIR);
        const std::string& name = applicationIdentity().name;
        if (!datadir.empty() && !datadir.is_absolute()) {
          appendUnique(candidates, exeDir.parent_path() / datadir / name / "assets");
        }
        appendUnique(candidates, exeDir.parent_path() / "share" / name / "assets");
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

    bool isLibraryAssetRoot(const std::filesystem::path& root) {
      std::error_code ec;
      return std::filesystem::exists(root / "fonts" / "noctalia-tabler.ttf", ec);
    }

    std::filesystem::path resolveLibraryAssetsRoot() {
      // Installed: the application installs lib/assets into its own asset directory.
      std::vector<std::filesystem::path> candidates{assetsRoot()};
      // Build tree: lib/assets beside the build directory, as the application's assets/ is.
      if (auto exe = executablePath()) {
        appendUnique(candidates, exe->parent_path().parent_path() / "lib" / "assets");
      }
      for (const auto& candidate : candidates) {
        if (isLibraryAssetRoot(candidate)) {
          kLog.debug("using library assets from {}", candidate.string());
          return candidate;
        }
      }
      kLog.warn("could not locate the library assets; defaulting to {}", assetsRoot().string());
      return assetsRoot();
    }

  } // namespace

  const std::filesystem::path& libraryAssetsRoot() {
    static const std::filesystem::path s_root = resolveLibraryAssetsRoot();
    return s_root;
  }

  std::filesystem::path libraryAssetPath(std::string_view relativePath) {
    return libraryAssetsRoot() / std::filesystem::path(std::string(relativePath));
  }

  const std::filesystem::path& assetsRoot() {
    static const std::filesystem::path s_root = resolveAssetsRoot();
    return s_root;
  }

  std::filesystem::path assetPath(std::string_view relativePath) {
    return assetsRoot() / std::filesystem::path(std::string(relativePath));
  }

} // namespace paths
