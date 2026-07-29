#include "shell/profile/avatar_path.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "core/log.h"
#include "dbus/accounts/accounts_service.h"
#include "render/core/image_encoder.h"
#include "render/core/image_file_loader.h"
#include "util/file_utils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace {

  constexpr Logger kLog("avatar");
  constexpr int kAccountsAvatarMaxSize = 512;
  // Sources this much larger than the accounts avatar decode noticeably slowly.
  constexpr int kAvatarSourceWarnSize = 4 * kAccountsAvatarMaxSize;

  [[nodiscard]] std::filesystem::path avatarCacheDirectory() {
    const char* cacheHome = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    if (cacheHome != nullptr && cacheHome[0] != '\0') {
      return std::filesystem::path(cacheHome) / "noctalia";
    }
    if (home != nullptr && home[0] != '\0') {
      return std::filesystem::path(home) / ".cache" / "noctalia";
    }
    return {};
  }

  enum class AvatarPrepareError {
    LoadFailed,
    WriteFailed,
  };

  [[nodiscard]] std::optional<std::filesystem::path> prepareAvatarForAccounts(
      const std::filesystem::path& sourcePath, AvatarPrepareError& errorOut, std::string* logDetail
  ) {
    const auto loaded = loadImageFile(sourcePath.string(), kAccountsAvatarMaxSize, true);
    if (!loaded) {
      errorOut = AvatarPrepareError::LoadFailed;
      if (logDetail != nullptr) {
        *logDetail = loaded.error();
      }
      return std::nullopt;
    }

    if (std::max(loaded->sourceWidth, loaded->sourceHeight) > kAvatarSourceWarnSize) {
      kLog.warn(
          "avatar source '{}' is {}x{}, much larger than needed; large images decode slowly", sourcePath.string(),
          loaded->sourceWidth, loaded->sourceHeight
      );
    }

    std::string encodeError;
    const std::vector<std::uint8_t> png = encodePng(loaded->rgba.data(), loaded->width, loaded->height, &encodeError);
    if (png.empty()) {
      errorOut = AvatarPrepareError::WriteFailed;
      if (logDetail != nullptr) {
        *logDetail = encodeError.empty() ? "failed to encode avatar image" : encodeError;
      }
      return std::nullopt;
    }

    const auto cacheDir = avatarCacheDirectory();
    if (cacheDir.empty()) {
      errorOut = AvatarPrepareError::WriteFailed;
      if (logDetail != nullptr) {
        *logDetail = "avatar cache directory is unavailable";
      }
      return std::nullopt;
    }

    std::error_code ec;
    std::filesystem::create_directories(cacheDir, ec);
    if (ec) {
      errorOut = AvatarPrepareError::WriteFailed;
      if (logDetail != nullptr) {
        *logDetail = ec.message();
      }
      return std::nullopt;
    }

    const auto destination = cacheDir / "avatar.png";
    const auto tempPath = cacheDir / "avatar.png.tmp";
    {
      std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
      if (!out.is_open()) {
        errorOut = AvatarPrepareError::WriteFailed;
        if (logDetail != nullptr) {
          *logDetail = "failed to open avatar cache file";
        }
        return std::nullopt;
      }
      out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
      if (!out) {
        errorOut = AvatarPrepareError::WriteFailed;
        if (logDetail != nullptr) {
          *logDetail = "failed to write avatar cache file";
        }
        std::filesystem::remove(tempPath, ec);
        return std::nullopt;
      }
    }

    ec.clear();
    std::filesystem::rename(tempPath, destination, ec);
    if (ec) {
      errorOut = AvatarPrepareError::WriteFailed;
      if (logDetail != nullptr) {
        *logDetail = ec.message();
      }
      std::filesystem::remove(tempPath, ec);
      return std::nullopt;
    }

    std::filesystem::permissions(
        destination, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ec
    );

    return destination;
  }

} // namespace

namespace shell {

  std::string resolvedAvatarPath(const AccountsService* accounts, const Config& config) {
    if (!config.shell.avatarPath.empty()) {
      return config.shell.avatarPath;
    }
    if (accounts != nullptr) {
      const std::string& iconFile = accounts->iconFile();
      if (!iconFile.empty()) {
        return iconFile;
      }
    }
    return {};
  }

  std::string avatarDisplayPath(const AccountsService* accounts, const Config& config) {
    return resolvedAvatarPath(accounts, config);
  }

  AvatarApplyResult applyAvatarPath(AccountsService* accounts, ConfigService* config, std::string_view path) {
    if (config == nullptr) {
      return {.error = AvatarApplyError::ConfigFailed};
    }

    const std::string normalizedPath = path.empty() ? std::string{} : FileUtils::normalizeWallpaperPath(path);
    if (!normalizedPath.empty()) {
      std::error_code ec;
      if (!std::filesystem::is_regular_file(FileUtils::expandUserPath(normalizedPath), ec) || ec) {
        kLog.warn("avatar path is not a regular file: {}", normalizedPath);
        return {.error = AvatarApplyError::InvalidPath};
      }
    }

    std::string accountsPath = normalizedPath;
    if (accounts != nullptr && !normalizedPath.empty()) {
      const auto sourcePath = FileUtils::expandUserPath(normalizedPath);
      AvatarPrepareError prepareError = AvatarPrepareError::LoadFailed;
      std::string prepareDetail;
      const auto prepared = prepareAvatarForAccounts(sourcePath, prepareError, &prepareDetail);
      if (!prepared.has_value()) {
        kLog.warn("failed to prepare avatar from '{}': {}", normalizedPath, prepareDetail);
        return {
            .error = prepareError == AvatarPrepareError::WriteFailed ? AvatarApplyError::WriteFailed
                                                                     : AvatarApplyError::LoadFailed
        };
      }
      accountsPath = prepared->string();
      if (!accounts->setIconFile(accountsPath)) {
        kLog.warn("AccountsService SetIconFile failed for '{}', falling back to config override", accountsPath);
      }
    }

    if (!config->setOverride({"shell", "avatar_path"}, normalizedPath)) {
      kLog.warn("failed to persist shell.avatar_path");
      return {.error = AvatarApplyError::ConfigFailed};
    }

    return {};
  }

  const char* avatarApplyErrorTranslationKey(AvatarApplyError error) noexcept {
    switch (error) {
    case AvatarApplyError::None:
      return "";
    case AvatarApplyError::InvalidPath:
      return "settings.errors.avatar-invalid-path";
    case AvatarApplyError::LoadFailed:
      return "settings.errors.avatar-load";
    case AvatarApplyError::WriteFailed:
      return "settings.errors.avatar-write";
    case AvatarApplyError::AccountsFailed:
      return "settings.errors.avatar-accounts";
    case AvatarApplyError::ConfigFailed:
      return "settings.errors.write";
    }
    return "settings.errors.write";
  }

} // namespace shell
