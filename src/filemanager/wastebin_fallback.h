#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <functional>
#include <gio/gio.h>
#include <optional>
#include <string>

namespace filemanager::wastebin_fallback {

  struct TrashFailureDiagnosis {
    bool eligibleForHomeFallback = false;
    std::filesystem::path mountRoot;
    std::filesystem::path trashDirPath;
    std::string summary;
    std::string technicalDetail;
  };

  struct ReservedTrashEntry {
    std::string trashName;
    std::filesystem::path infoPath;
    std::filesystem::path filesPath;
  };

  [[nodiscard]] std::filesystem::path homeWastebinRoot();
  [[nodiscard]] std::filesystem::path mountRootForPath(const std::filesystem::path& path);
  [[nodiscard]] bool productionWastebinForbidden() noexcept;
  [[nodiscard]] bool validateIsolatedHomeWastebinRoot(const std::filesystem::path& trashRoot, std::string& reasonOut);
  [[nodiscard]] bool validateIsolatedTrashTarget(const std::filesystem::path& sourcePath, std::string& reasonOut);

  [[nodiscard]] TrashFailureDiagnosis
  diagnoseTrashFailure(const std::filesystem::path& sourcePath, const GError* gioError);

  bool ensureHomeWastebinDirs(
      std::filesystem::path& rootOut, std::filesystem::path& infoDirOut, std::filesystem::path& filesDirOut,
      GError** error
  );

  [[nodiscard]] std::string encodeTrashInfoPath(const std::filesystem::path& absolutePath);
  [[nodiscard]] std::string formatTrashDeletionDate(std::time_t timestamp);

  bool
  reserveHomeTrashEntry(const std::filesystem::path& originalAbsolutePath, ReservedTrashEntry& out, GError** error);

  [[nodiscard]] std::optional<std::uint64_t> queryAvailableBytes(const std::filesystem::path& path);
  [[nodiscard]] std::string formatByteSize(std::uint64_t bytes);

  [[nodiscard]] int invokeTrashHook(const char* sourcePath) noexcept;

} // namespace filemanager::wastebin_fallback

void setWastebinAvailableSpaceHookForTesting(std::function<std::optional<std::uint64_t>(const char*)> hook);
void setFileOperationTrashHookForTesting(std::function<int(const char*)> hook);
