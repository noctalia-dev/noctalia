#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <gio/gio.h>
#include <optional>
#include <string>
#include <sys/types.h>

namespace filemanager::fs_compat {

  enum class CapabilitySupport : std::uint8_t {
    Unknown,
    Supported,
    Unsupported,
  };

  struct FsCapabilities {
    CapabilitySupport renameNoReplace = CapabilitySupport::Unknown;
    CapabilitySupport hardLink = CapabilitySupport::Unknown;
  };

  struct EntryIdentity {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint32_t mode = 0;
    std::uint64_t links = 0;
  };

  // RAII parent directory pin (O_PATH | O_DIRECTORY) paired with a validated single leaf name.
  class PinnedEntry {
  public:
    PinnedEntry() = default;
    ~PinnedEntry();
    PinnedEntry(const PinnedEntry&) = delete;
    PinnedEntry& operator=(const PinnedEntry&) = delete;
    PinnedEntry(PinnedEntry&& other) noexcept;
    PinnedEntry& operator=(PinnedEntry&& other) noexcept;

    [[nodiscard]] static std::optional<PinnedEntry>
    open(const char* fullPath, int& savedErrno, const char* hookPath = nullptr);
    [[nodiscard]] static std::optional<PinnedEntry>
    fromBorrowedDirFd(int borrowedDirFd, const char* fullPath, const char* hookPath = nullptr);

    [[nodiscard]] int dirFd() const noexcept { return m_ownedFd >= 0 ? m_ownedFd : m_borrowedFd; }
    [[nodiscard]] const std::string& leaf() const noexcept { return m_leaf; }
    [[nodiscard]] const std::string& fullPath() const noexcept { return m_fullPath; }
    [[nodiscard]] const std::string& hookPath() const noexcept { return m_hookPath.empty() ? m_fullPath : m_hookPath; }

  private:
    int m_ownedFd = -1;
    int m_borrowedFd = -1;
    std::string m_leaf;
    std::string m_fullPath;
    std::string m_hookPath;
  };

  struct PublishOutcome {
    bool ok = false;
    bool destinationCommitted = false;
    bool sourceRemoved = false;
    bool splitState = false;
    bool unsupported = false;
    int savedErrno = 0;
    std::string message;
  };

  struct ReplacementBackup {
    std::filesystem::path targetPath;
    std::filesystem::path backupPath;
    std::filesystem::path containerPath;
    std::optional<EntryIdentity> expectedTarget;
    bool usedPrivateContainer = false;
  };

  [[nodiscard]] bool isNoReplaceUnsupportedErrno(int err) noexcept;
  [[nodiscard]] bool isHardLinkUnsupportedErrno(int err) noexcept;
  [[nodiscard]] bool isAmbiguousNetworkErrno(int err) noexcept;
  [[nodiscard]] bool sameEntryIdentity(const EntryIdentity& lhs, const EntryIdentity& rhs) noexcept;

  [[nodiscard]] std::optional<EntryIdentity> queryEntryIdentity(const char* path) noexcept;
  [[nodiscard]] std::optional<EntryIdentity> queryEntryIdentityAt(int dirFd, const char* leaf) noexcept;

  void invalidateCapabilityCache();
  [[nodiscard]] FsCapabilities probeCapabilities(const std::filesystem::path& directory);
  bool preflightTransferDestination(
      const std::filesystem::path& destinationDirectory, bool isDirectory, bool isSymlink, bool replaceExisting,
      GError** error
  );

  bool renameNoReplaceRaw(const char* sourcePath, const char* destinationPath, int& savedErrno, GError** error);

  bool createDirectoryExclusive(const char* path, mode_t mode, int& savedErrno, GError** error);
  bool createDirectoryExclusiveAt(int dirFd, const char* leaf, mode_t mode, int& savedErrno, GError** error);

  bool writeBufferToNewFileExclusive(
      const char* path, const std::uint8_t* data, std::size_t size, GCancellable* cancellable, bool& created,
      GError** error, const char* hookPath = nullptr, int borrowedDirFd = -1
  );

  bool
  unlinkVerifiedAt(int dirFd, const char* leaf, const EntryIdentity& expected, const char* hookPath, int& savedErrno);
  bool removeEntryHookActive() noexcept;
  int invokeRemoveEntryHook(const char* path) noexcept;

  PublishOutcome moveOrPublishNonDirectory(
      const char* srcPath, const char* dstPath, bool isStagedTemp, const std::optional<EntryIdentity>& expectedSource,
      bool allowSymlinkCreateFallback, const char* srcHookPath = nullptr, const char* dstHookPath = nullptr,
      int borrowedSrcDirFd = -1, int borrowedDstDirFd = -1
  );

  PublishOutcome moveOrPublishDirectoryOptionA(
      const char* srcPath, const char* dstPath, bool isStagedTemp, const std::optional<EntryIdentity>& expectedSource,
      const char* srcHookPath = nullptr, const char* dstHookPath = nullptr, int borrowedSrcDirFd = -1,
      int borrowedDstDirFd = -1
  );

  PublishOutcome moveOrPublishEntryNoReplace(
      const char* srcPath, const char* dstPath, bool isStagedTemp, const std::optional<EntryIdentity>& expectedSource,
      bool allowSymlinkCreateFallback, const char* srcHookPath = nullptr, const char* dstHookPath = nullptr,
      int borrowedSrcDirFd = -1, int borrowedDstDirFd = -1
  );

  bool verifyReplaceableTarget(const char* targetPath, const std::optional<EntryIdentity>& expected, GError** error);
  bool allocateReplacementBackup(
      const char* targetPath, const std::optional<EntryIdentity>& expectedTarget,
      std::optional<ReplacementBackup>& backup, std::string& message, GError** error
  );
  void restoreOrReportBackup(
      const ReplacementBackup& backup, const char* fallbackError, GError* error, std::string& message
  );
  bool removeOrReportBackup(const ReplacementBackup& backup, const char* prefix, std::string& message);

} // namespace filemanager::fs_compat
