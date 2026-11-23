#include "filemanager/fs_compat.h"

#include "filemanager/file_interaction.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <mutex>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
  std::function<int(const char*, const char*)> g_renameNoReplaceTestHook;
  std::function<void(const char*, const char*, bool)> g_replaceBackupTestHook;
  std::function<int(const char*)> g_removeEntryTestHook;
  std::function<int(const char*, const char*)> g_linkAtTestHook;
  std::function<int(const char*, const char*)> g_symlinkAtTestHook;
  std::function<int(const char*, const char*)> g_directoryRenameTestHook;

  struct CapabilityCacheKey {
    std::uint64_t device = 0;
    std::uint64_t mountId = 0;
    unsigned long fsType = 0;
    bool operator==(const CapabilityCacheKey&) const = default;
  };

  struct CapabilityCacheEntry {
    CapabilityCacheKey key;
    filemanager::fs_compat::FsCapabilities caps;
    std::chrono::steady_clock::time_point expires;
  };

  std::mutex g_capabilityCacheMutex;
  std::vector<CapabilityCacheEntry> g_capabilityCache;
  constexpr auto kCapabilityCacheTtl = std::chrono::seconds(60);

  bool anyCompatHookInstalled() noexcept {
    return static_cast<bool>(g_renameNoReplaceTestHook)
        || static_cast<bool>(g_linkAtTestHook)
        || static_cast<bool>(g_symlinkAtTestHook)
        || static_cast<bool>(g_directoryRenameTestHook);
  }

  bool splitParentAndLeaf(const char* rawPath, std::string& parentOut, std::string& leafOut) {
    if (rawPath == nullptr || rawPath[0] == '\0')
      return false;
    std::string normalized(rawPath);
    while (normalized.size() > 1 && normalized.back() == '/')
      normalized.pop_back();
    if (normalized == "/")
      return false;
    const auto slash = normalized.rfind('/');
    if (slash == std::string::npos) {
      parentOut = ".";
      leafOut = std::move(normalized);
    } else {
      parentOut = slash == 0 ? std::string("/") : normalized.substr(0, slash);
      leafOut = normalized.substr(slash + 1);
    }
    return filemanager::validFileName(leafOut);
  }

  filemanager::fs_compat::EntryIdentity fromStat(const struct stat& state) noexcept {
    return {
        .device = static_cast<std::uint64_t>(state.st_dev),
        .inode = static_cast<std::uint64_t>(state.st_ino),
        .mode = static_cast<std::uint32_t>(state.st_mode),
        .links = static_cast<std::uint64_t>(state.st_nlink),
    };
  }

  std::optional<filemanager::fs_compat::EntryIdentity>
  probeEntryAt(int dirFd, const char* leaf, int& probeErrno) noexcept {
    probeErrno = 0;
    if (dirFd < 0 || leaf == nullptr || leaf[0] == '\0') {
      probeErrno = EINVAL;
      return std::nullopt;
    }
    struct stat state{};
    if (fstatat(dirFd, leaf, &state, AT_SYMLINK_NOFOLLOW) != 0) {
      probeErrno = errno;
      return std::nullopt;
    }
    return fromStat(state);
  }

  std::optional<CapabilityCacheKey> queryDirectoryCacheKey(int dirFd, const std::filesystem::path& directory) {
    struct statx stx{};
    if (statx(dirFd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS | STATX_MNT_ID, &stx) != 0)
      return std::nullopt;
    const std::uint64_t device = (static_cast<std::uint64_t>(stx.stx_dev_major) << 32U) | stx.stx_dev_minor;
    const std::uint64_t mountId = (stx.stx_mask & STATX_MNT_ID) ? stx.stx_mnt_id : 0ULL;
    struct statfs sfs{};
    const std::string procFd = "/proc/self/fd/" + std::to_string(dirFd);
    unsigned long fsType = 0;
    if (statfs(procFd.c_str(), &sfs) == 0 || statfs(directory.c_str(), &sfs) == 0)
      fsType = static_cast<unsigned long>(sfs.f_type);
    return CapabilityCacheKey{device, mountId, fsType};
  }

  bool readSymlinkAtPath(int dirFd, const char* leaf, std::string& targetOut, int& savedErrno) {
    targetOut.clear();
    std::vector<char> buffer(PATH_MAX);
    while (true) {
      const ssize_t len = readlinkat(dirFd, leaf, buffer.data(), buffer.size());
      if (len < 0) {
        savedErrno = errno;
        return false;
      }
      if (static_cast<std::size_t>(len) < buffer.size()) {
        targetOut.assign(buffer.data(), static_cast<std::size_t>(len));
        return true;
      }
      if (buffer.size() >= 1024U * 1024U) {
        savedErrno = ENAMETOOLONG;
        return false;
      }
      buffer.resize(buffer.size() * 2U);
    }
  }

  void setErrorFromErrno(GError** error, int savedErrno, const char* unsupportedOverride = nullptr) {
    if (error == nullptr)
      return;
    if (unsupportedOverride != nullptr && filemanager::fs_compat::isNoReplaceUnsupportedErrno(savedErrno)) {
      const GIOErrorEnum code = savedErrno == EINVAL ? G_IO_ERROR_INVALID_ARGUMENT : G_IO_ERROR_NOT_SUPPORTED;
      g_set_error_literal(error, G_IO_ERROR, code, unsupportedOverride);
      return;
    }
    const GIOErrorEnum code = savedErrno == EXDEV ? G_IO_ERROR_NOT_SUPPORTED : g_io_error_from_errno(savedErrno);
    g_set_error_literal(error, G_IO_ERROR, code, g_strerror(savedErrno));
  }

  int callRenameNoReplace(
      const filemanager::fs_compat::PinnedEntry& src, const filemanager::fs_compat::PinnedEntry& dst
  ) {
    if (g_renameNoReplaceTestHook)
      return g_renameNoReplaceTestHook(src.hookPath().c_str(), dst.hookPath().c_str());
    return static_cast<int>(
        syscall(SYS_renameat2, src.dirFd(), src.leaf().c_str(), dst.dirFd(), dst.leaf().c_str(), RENAME_NOREPLACE)
    );
  }

  int callLinkAt(const filemanager::fs_compat::PinnedEntry& src, const filemanager::fs_compat::PinnedEntry& dst) {
    if (g_linkAtTestHook)
      return g_linkAtTestHook(src.hookPath().c_str(), dst.hookPath().c_str());
    return linkat(src.dirFd(), src.leaf().c_str(), dst.dirFd(), dst.leaf().c_str(), 0);
  }

  int callSymlinkAt(const char* linkTarget, const filemanager::fs_compat::PinnedEntry& dst) {
    if (g_symlinkAtTestHook)
      return g_symlinkAtTestHook(linkTarget, dst.hookPath().c_str());
    return symlinkat(linkTarget, dst.dirFd(), dst.leaf().c_str());
  }

  int callDirectoryRename(
      const filemanager::fs_compat::PinnedEntry& src, const filemanager::fs_compat::PinnedEntry& dst
  ) {
    if (g_directoryRenameTestHook)
      return g_directoryRenameTestHook(src.hookPath().c_str(), dst.hookPath().c_str());
    return renameat(src.dirFd(), src.leaf().c_str(), dst.dirFd(), dst.leaf().c_str());
  }

  int callUnlinkAt(int dirFd, const char* leaf, const char* hookPath) {
    if (g_removeEntryTestHook && hookPath != nullptr) {
      if (const int injected = g_removeEntryTestHook(hookPath); injected != 0) {
        errno = injected;
        return -1;
      }
    }
    return unlinkat(dirFd, leaf, 0);
  }
} // namespace

namespace filemanager::fs_compat {

  PinnedEntry::~PinnedEntry() {
    if (m_ownedFd >= 0)
      close(m_ownedFd);
  }

  PinnedEntry::PinnedEntry(PinnedEntry&& other) noexcept
      : m_ownedFd(std::exchange(other.m_ownedFd, -1)), m_borrowedFd(std::exchange(other.m_borrowedFd, -1)),
        m_leaf(std::move(other.m_leaf)), m_fullPath(std::move(other.m_fullPath)),
        m_hookPath(std::move(other.m_hookPath)) {}

  PinnedEntry& PinnedEntry::operator=(PinnedEntry&& other) noexcept {
    if (this != &other) {
      if (m_ownedFd >= 0)
        close(m_ownedFd);
      m_ownedFd = std::exchange(other.m_ownedFd, -1);
      m_borrowedFd = std::exchange(other.m_borrowedFd, -1);
      m_leaf = std::move(other.m_leaf);
      m_fullPath = std::move(other.m_fullPath);
      m_hookPath = std::move(other.m_hookPath);
    }
    return *this;
  }

  std::optional<PinnedEntry> PinnedEntry::open(const char* fullPath, int& savedErrno, const char* hookPath) {
    savedErrno = 0;
    std::string parent;
    std::string leaf;
    if (!splitParentAndLeaf(fullPath, parent, leaf)) {
      savedErrno = EINVAL;
      return std::nullopt;
    }
    const int fd = ::open(parent.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
      savedErrno = errno;
      return std::nullopt;
    }
    PinnedEntry entry;
    entry.m_ownedFd = fd;
    entry.m_leaf = std::move(leaf);
    entry.m_fullPath = fullPath;
    if (hookPath != nullptr)
      entry.m_hookPath = hookPath;
    return entry;
  }

  std::optional<PinnedEntry>
  PinnedEntry::fromBorrowedDirFd(int borrowedDirFd, const char* fullPath, const char* hookPath) {
    if (borrowedDirFd < 0)
      return std::nullopt;
    std::string parent;
    std::string leaf;
    if (!splitParentAndLeaf(fullPath, parent, leaf))
      return std::nullopt;
    PinnedEntry entry;
    entry.m_borrowedFd = borrowedDirFd;
    entry.m_leaf = std::move(leaf);
    entry.m_fullPath = fullPath;
    if (hookPath != nullptr)
      entry.m_hookPath = hookPath;
    return entry;
  }

  bool isNoReplaceUnsupportedErrno(int err) noexcept {
    return err == ENOSYS || err == EINVAL || err == EOPNOTSUPP || err == ENOTSUP;
  }

  bool isHardLinkUnsupportedErrno(int err) noexcept {
    return err == EPERM || err == EOPNOTSUPP || err == ENOTSUP || err == ENOSYS || err == EMLINK;
  }

  bool isAmbiguousNetworkErrno(int err) noexcept {
    return err == EIO
        || err == ESTALE
        || err == ETIMEDOUT
        || err == EINTR
#ifdef EREMOTEIO
        || err == EREMOTEIO
#endif
        ;
  }

  bool sameEntryIdentity(const EntryIdentity& lhs, const EntryIdentity& rhs) noexcept {
    return lhs.device != 0
        && lhs.device == rhs.device
        && lhs.inode == rhs.inode
        && (lhs.mode & S_IFMT) == (rhs.mode & S_IFMT);
  }

  std::optional<EntryIdentity> queryEntryIdentity(const char* path) noexcept {
    if (path == nullptr || path[0] == '\0')
      return std::nullopt;
    struct stat state{};
    if (lstat(path, &state) != 0)
      return std::nullopt;
    return fromStat(state);
  }

  std::optional<EntryIdentity> queryEntryIdentityAt(int dirFd, const char* leaf) noexcept {
    if (dirFd < 0 || leaf == nullptr || leaf[0] == '\0')
      return std::nullopt;
    struct stat state{};
    if (fstatat(dirFd, leaf, &state, AT_SYMLINK_NOFOLLOW) != 0)
      return std::nullopt;
    return fromStat(state);
  }

  void invalidateCapabilityCache() {
    std::lock_guard lock(g_capabilityCacheMutex);
    g_capabilityCache.clear();
  }

  FsCapabilities probeCapabilities(const std::filesystem::path& directory) {
    FsCapabilities caps;
    if (directory.empty())
      return caps;

    const bool useCache = !anyCompatHookInstalled();
    const int dirFd = ::open(directory.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (dirFd < 0)
      return caps;

    std::optional<CapabilityCacheKey> cacheKey;
    const auto now = std::chrono::steady_clock::now();
    if (useCache) {
      cacheKey = queryDirectoryCacheKey(dirFd, directory);
      if (cacheKey) {
        std::lock_guard lock(g_capabilityCacheMutex);
        for (const auto& cached : g_capabilityCache) {
          if (cached.key == *cacheKey && cached.expires > now) {
            close(dirFd);
            return cached.caps;
          }
        }
      }
    }
    close(dirFd);

    // Probe inside a fresh private 0700 directory against an absent sibling name
    // so EEXIST is never misread as RENAME_NOREPLACE support.
    std::string probeDir = (directory / ".nythera-cap-XXXXXX").string();
    if (g_mkdtemp(probeDir.data()) == nullptr)
      return caps;
    const int probeFd = ::open(probeDir.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (probeFd < 0) {
      rmdir(probeDir.c_str());
      return caps;
    }

    const int createdFd = openat(probeFd, "a", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (createdFd >= 0) {
      close(createdFd);
      const std::string pathA = probeDir + "/a";
      const std::string pathB = probeDir + "/b";
      const int renameRc = g_renameNoReplaceTestHook
          ? g_renameNoReplaceTestHook(pathA.c_str(), pathB.c_str())
          : static_cast<int>(syscall(SYS_renameat2, probeFd, "a", probeFd, "b", RENAME_NOREPLACE));
      if (renameRc == 0) {
        caps.renameNoReplace = CapabilitySupport::Supported;
        unlinkat(probeFd, "b", 0);
      } else {
        const int renameErrno = errno;
        if (isNoReplaceUnsupportedErrno(renameErrno)) {
          caps.renameNoReplace = CapabilitySupport::Unsupported;
          const int linkRc =
              g_linkAtTestHook ? g_linkAtTestHook(pathA.c_str(), pathB.c_str()) : linkat(probeFd, "a", probeFd, "b", 0);
          if (linkRc == 0) {
            caps.hardLink = CapabilitySupport::Supported;
            unlinkat(probeFd, "b", 0);
          } else if (isHardLinkUnsupportedErrno(errno)) {
            caps.hardLink = CapabilitySupport::Unsupported;
          }
        }
        unlinkat(probeFd, "a", 0);
      }
    }
    close(probeFd);
    rmdir(probeDir.c_str());

    if (useCache && cacheKey && caps.renameNoReplace != CapabilitySupport::Unknown) {
      std::lock_guard lock(g_capabilityCacheMutex);
      std::erase_if(g_capabilityCache, [&](const auto& entry) {
        return entry.key == *cacheKey || entry.expires <= now;
      });
      g_capabilityCache.push_back({*cacheKey, caps, now + kCapabilityCacheTtl});
    }
    return caps;
  }

  bool preflightTransferDestination(
      const std::filesystem::path& destinationDirectory, bool isDirectory, bool isSymlink, bool replaceExisting,
      GError** error
  ) {
    const auto caps = probeCapabilities(destinationDirectory);
    if (caps.renameNoReplace != CapabilitySupport::Unsupported)
      return true;
    if (isDirectory && !replaceExisting)
      return true;
    if (isSymlink && !replaceExisting)
      return true;
    if (caps.hardLink != CapabilitySupport::Unsupported)
      return true;
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
        "The filesystem does not support atomic non-overwriting moves (RENAME_NOREPLACE) or hard-link publication."
    );
    return false;
  }

  bool renameNoReplaceRaw(const char* sourcePath, const char* destinationPath, int& savedErrno, GError** error) {
    savedErrno = 0;
    if (sourcePath == nullptr || destinationPath == nullptr) {
      savedErrno = EOPNOTSUPP;
      g_set_error_literal(
          error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Atomic non-overwriting move requires local filesystem paths."
      );
      return false;
    }
    const int rc = g_renameNoReplaceTestHook
        ? g_renameNoReplaceTestHook(sourcePath, destinationPath)
        : static_cast<int>(syscall(SYS_renameat2, AT_FDCWD, sourcePath, AT_FDCWD, destinationPath, RENAME_NOREPLACE));
    if (rc == 0)
      return true;
    savedErrno = errno;
    setErrorFromErrno(
        error, savedErrno, "The filesystem does not support atomic non-overwriting moves (RENAME_NOREPLACE)."
    );
    return false;
  }

  bool createDirectoryExclusive(const char* path, mode_t mode, int& savedErrno, GError** error) {
    savedErrno = 0;
    auto pinned = PinnedEntry::open(path, savedErrno);
    if (!pinned) {
      setErrorFromErrno(error, savedErrno);
      return false;
    }
    return createDirectoryExclusiveAt(pinned->dirFd(), pinned->leaf().c_str(), mode, savedErrno, error);
  }

  bool createDirectoryExclusiveAt(int dirFd, const char* leaf, mode_t mode, int& savedErrno, GError** error) {
    savedErrno = 0;
    if (mkdirat(dirFd, leaf, mode) == 0)
      return true;
    savedErrno = errno;
    setErrorFromErrno(error, savedErrno);
    return false;
  }

  bool writeBufferToNewFileExclusive(
      const char* path, const std::uint8_t* data, std::size_t size, GCancellable* cancellable, bool& created,
      GError** error, const char* hookPath, int borrowedDirFd
  ) {
    if (g_cancellable_set_error_if_cancelled(cancellable, error))
      return false;
    int savedErrno = 0;
    auto pinned = borrowedDirFd >= 0 ? PinnedEntry::fromBorrowedDirFd(borrowedDirFd, path, hookPath)
                                     : PinnedEntry::open(path, savedErrno, hookPath);
    if (!pinned) {
      setErrorFromErrno(error, savedErrno ? savedErrno : EINVAL);
      return false;
    }
    const int fd =
        openat(pinned->dirFd(), pinned->leaf().c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
    if (fd < 0) {
      setErrorFromErrno(error, errno);
      return false;
    }
    created = true;
    struct stat st{};
    std::optional<EntryIdentity> createdId;
    if (fstat(fd, &st) == 0)
      createdId = fromStat(st);

    bool ok = true;
    std::size_t offset = 0;
    while (offset < size) {
      if (g_cancellable_set_error_if_cancelled(cancellable, error)) {
        ok = false;
        break;
      }
      const std::size_t chunk = std::min<std::size_t>(size - offset, 65536U);
      const ssize_t written = write(fd, data + offset, chunk);
      if (written > 0) {
        offset += static_cast<std::size_t>(written);
      } else if (written < 0 && errno == EINTR) {
        continue;
      } else {
        setErrorFromErrno(error, written < 0 ? errno : EIO);
        ok = false;
        break;
      }
    }
    if (close(fd) != 0 && ok) {
      setErrorFromErrno(error, errno);
      ok = false;
    }
    if (!ok && createdId) {
      int cleanupErrno = 0;
      if (unlinkVerifiedAt(
              pinned->dirFd(), pinned->leaf().c_str(), *createdId, pinned->hookPath().c_str(), cleanupErrno
          ))
        created = false;
    }
    return ok;
  }

  bool
  unlinkVerifiedAt(int dirFd, const char* leaf, const EntryIdentity& expected, const char* hookPath, int& savedErrno) {
    savedErrno = 0;
    const auto current = queryEntryIdentityAt(dirFd, leaf);
    if (!current || !sameEntryIdentity(*current, expected)) {
      savedErrno = EAGAIN;
      return false;
    }
    if (callUnlinkAt(dirFd, leaf, hookPath) == 0)
      return true;
    savedErrno = errno;
    return false;
  }

  bool removeEntryHookActive() noexcept { return static_cast<bool>(g_removeEntryTestHook); }

  int invokeRemoveEntryHook(const char* path) noexcept {
    if (!g_removeEntryTestHook || path == nullptr)
      return 0;
    return g_removeEntryTestHook(path);
  }

  PublishOutcome moveOrPublishNonDirectory(
      const char* srcPath, const char* dstPath, bool isStagedTemp, const std::optional<EntryIdentity>& expectedSource,
      bool allowSymlinkCreateFallback, const char* srcHookPath, const char* dstHookPath, int borrowedSrcDirFd,
      int borrowedDstDirFd
  ) {
    PublishOutcome out;
    int srcOpenErr = 0;
    int dstOpenErr = 0;
    auto src = borrowedSrcDirFd >= 0 ? PinnedEntry::fromBorrowedDirFd(borrowedSrcDirFd, srcPath, srcHookPath)
                                     : PinnedEntry::open(srcPath, srcOpenErr, srcHookPath);
    if (!src) {
      out.savedErrno = srcOpenErr ? srcOpenErr : EINVAL;
      out.message = g_strerror(out.savedErrno);
      return out;
    }
    auto dst = borrowedDstDirFd >= 0 ? PinnedEntry::fromBorrowedDirFd(borrowedDstDirFd, dstPath, dstHookPath)
                                     : PinnedEntry::open(dstPath, dstOpenErr, dstHookPath);
    if (!dst) {
      out.savedErrno = dstOpenErr ? dstOpenErr : EINVAL;
      out.message = g_strerror(out.savedErrno);
      return out;
    }

    const auto srcIdentity = queryEntryIdentityAt(src->dirFd(), src->leaf().c_str());
    if (!srcIdentity || (expectedSource && !sameEntryIdentity(*srcIdentity, *expectedSource))) {
      out.savedErrno = srcIdentity ? EAGAIN : ENOENT;
      out.message = "The source item changed or is missing before finalisation.";
      return out;
    }
    if (S_ISDIR(srcIdentity->mode)) {
      out.savedErrno = EISDIR;
      out.message = g_strerror(EISDIR);
      return out;
    }

    // 1. Fast path: atomic RENAME_NOREPLACE whenever supported.
    if (callRenameNoReplace(*src, *dst) == 0) {
      out.ok = true;
      out.destinationCommitted = true;
      out.sourceRemoved = true;
      return out;
    }
    const int renameErr = errno;
    if (!isNoReplaceUnsupportedErrno(renameErr)) {
      if (isAmbiguousNetworkErrno(renameErr)) {
        const auto dstNow = queryEntryIdentityAt(dst->dirFd(), dst->leaf().c_str());
        const auto srcNow = queryEntryIdentityAt(src->dirFd(), src->leaf().c_str());
        if (dstNow && sameEntryIdentity(*dstNow, *srcIdentity)) {
          if (!srcNow) {
            out.ok = true;
            out.destinationCommitted = true;
            out.sourceRemoved = true;
            return out;
          }
          out.destinationCommitted = true;
          out.splitState = true;
          out.savedErrno = renameErr;
          out.message = "Moved copy exists at " + dst->fullPath() + "; source still exists at " + src->fullPath();
          return out;
        }
      }
      out.savedErrno = renameErr;
      out.message = g_strerror(renameErr);
      return out;
    }

    // 2. Fallback path for filesystems lacking RENAME_NOREPLACE:
    //    linkat(..., 0) is atomic against destination collisions (including dangling symlinks).
    bool committedViaLink = false;
    bool committedViaSymlinkCreate = false;
    int linkErr = 0;
    if (callLinkAt(*src, *dst) == 0) {
      committedViaLink = true;
    } else {
      linkErr = errno;
      if (linkErr == EEXIST && isStagedTemp) {
        const auto dstNow = queryEntryIdentityAt(dst->dirFd(), dst->leaf().c_str());
        if (dstNow && sameEntryIdentity(*dstNow, *srcIdentity))
          committedViaLink = true;
      } else if (isAmbiguousNetworkErrno(linkErr)) {
        const auto dstNow = queryEntryIdentityAt(dst->dirFd(), dst->leaf().c_str());
        if (dstNow && sameEntryIdentity(*dstNow, *srcIdentity))
          committedViaLink = true;
      }
    }

    if (!committedViaLink) {
      if (S_ISLNK(srcIdentity->mode) && isHardLinkUnsupportedErrno(linkErr) && allowSymlinkCreateFallback) {
        std::string linkTarget;
        int readErr = 0;
        struct stat sourceLink{};
        if (fstatat(src->dirFd(), src->leaf().c_str(), &sourceLink, AT_SYMLINK_NOFOLLOW) != 0
            || !sameEntryIdentity(fromStat(sourceLink), *srcIdentity)
            || !readSymlinkAtPath(src->dirFd(), src->leaf().c_str(), linkTarget, readErr)) {
          out.savedErrno = readErr ? readErr : EAGAIN;
          out.message = "The source symbolic link changed before finalisation.";
          return out;
        }
        const bool createdLink = callSymlinkAt(linkTarget.c_str(), *dst) == 0;
        if (createdLink) {
          committedViaSymlinkCreate = true;
        } else {
          const int symErr = errno;
          if (isAmbiguousNetworkErrno(symErr)) {
            std::string existingTarget;
            int readExistingErr = 0;
            const auto dstNow = queryEntryIdentityAt(dst->dirFd(), dst->leaf().c_str());
            if (dstNow
                && S_ISLNK(dstNow->mode)
                && readSymlinkAtPath(dst->dirFd(), dst->leaf().c_str(), existingTarget, readExistingErr)
                && existingTarget == linkTarget) {
              committedViaSymlinkCreate = true;
            }
          }
          if (!committedViaSymlinkCreate) {
            out.savedErrno = symErr;
            out.message = g_strerror(symErr);
            return out;
          }
        }
        // Recreating the link applies destination ownership defaults. Match the
        // original link before removing it, without following either link.
        int metadataErr = 0;
        const int linkFd = openat(dst->dirFd(), dst->leaf().c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
        if (linkFd < 0) {
          metadataErr = errno;
        } else {
          struct stat destinationLink{};
          std::string destinationTarget;
          if (fstat(linkFd, &destinationLink) != 0) {
            metadataErr = errno;
          } else if (!S_ISLNK(destinationLink.st_mode)
                     || !readSymlinkAtPath(linkFd, "", destinationTarget, metadataErr)
                     || destinationTarget != linkTarget) {
            metadataErr = EAGAIN;
          } else if (destinationLink.st_uid != sourceLink.st_uid || destinationLink.st_gid != sourceLink.st_gid) {
            if (!createdLink || destinationLink.st_uid != geteuid())
              metadataErr = EAGAIN;
            else if (fchownat(linkFd, "", sourceLink.st_uid, sourceLink.st_gid, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW) != 0
                     || fstat(linkFd, &destinationLink) != 0)
              metadataErr = errno;
            else if (destinationLink.st_uid != sourceLink.st_uid || destinationLink.st_gid != sourceLink.st_gid)
              metadataErr = EOPNOTSUPP;
          }
          close(linkFd);
        }
        if (metadataErr != 0) {
          out.destinationCommitted = true;
          out.splitState = true;
          out.metadataFailed = true;
          out.savedErrno = metadataErr;
          out.message = std::string("Recreated symbolic link, but could not preserve its owner or group: ")
              + g_strerror(metadataErr) + "; source remains at " + src->fullPath();
          return out;
        }
      } else if (isHardLinkUnsupportedErrno(linkErr)) {
        out.unsupported = true;
        out.savedErrno = linkErr;
        out.message =
            "The filesystem does not support atomic non-overwriting moves (RENAME_NOREPLACE) or hard-link publication.";
        return out;
      } else {
        out.savedErrno = linkErr;
        out.message = g_strerror(linkErr);
        return out;
      }
    }

    out.destinationCommitted = true;

    // 3. Verified removal of staged temp or move source after atomic destination commit.
    int unlinkErr = 0;
    if (unlinkVerifiedAt(src->dirFd(), src->leaf().c_str(), *srcIdentity, src->hookPath().c_str(), unlinkErr)) {
      out.ok = true;
      out.sourceRemoved = true;
      return out;
    }

    // Never report success if the source/staging removal failed unless an interrupted
    // unlink actually removed our exact entry and destination holds the committed item.
    if (unlinkErr == ENOENT) {
      const auto srcNow = queryEntryIdentityAt(src->dirFd(), src->leaf().c_str());
      const auto dstNow = queryEntryIdentityAt(dst->dirFd(), dst->leaf().c_str());
      if (!srcNow && dstNow && (committedViaSymlinkCreate || sameEntryIdentity(*dstNow, *srcIdentity))) {
        out.ok = true;
        out.sourceRemoved = true;
        return out;
      }
    }

    out.savedErrno = unlinkErr;
    out.splitState = true;
    out.sourceRemoved = false;
    if (isStagedTemp) {
      out.message = "Published item to "
          + dst->fullPath()
          + ", but could not remove temporary staging entry at "
          + src->fullPath()
          + ": "
          + g_strerror(unlinkErr);
    } else {
      out.message = "Moved copy remains at "
          + dst->fullPath()
          + "; source still exists at "
          + src->fullPath()
          + ": "
          + g_strerror(unlinkErr);
    }
    return out;
  }

  PublishOutcome moveOrPublishDirectoryOptionA(
      const char* srcPath, const char* dstPath, bool, const std::optional<EntryIdentity>& expectedSource,
      const char* srcHookPath, const char* dstHookPath, int borrowedSrcDirFd, int borrowedDstDirFd
  ) {
    PublishOutcome out;
    int srcOpenErr = 0;
    int dstOpenErr = 0;
    auto src = borrowedSrcDirFd >= 0 ? PinnedEntry::fromBorrowedDirFd(borrowedSrcDirFd, srcPath, srcHookPath)
                                     : PinnedEntry::open(srcPath, srcOpenErr, srcHookPath);
    if (!src) {
      out.savedErrno = srcOpenErr ? srcOpenErr : EINVAL;
      out.message = g_strerror(out.savedErrno);
      return out;
    }
    auto dst = borrowedDstDirFd >= 0 ? PinnedEntry::fromBorrowedDirFd(borrowedDstDirFd, dstPath, dstHookPath)
                                     : PinnedEntry::open(dstPath, dstOpenErr, dstHookPath);
    if (!dst) {
      out.savedErrno = dstOpenErr ? dstOpenErr : EINVAL;
      out.message = g_strerror(out.savedErrno);
      return out;
    }

    const auto srcIdentity = queryEntryIdentityAt(src->dirFd(), src->leaf().c_str());
    if (!srcIdentity || (expectedSource && !sameEntryIdentity(*srcIdentity, *expectedSource))) {
      out.savedErrno = srcIdentity ? EAGAIN : ENOENT;
      out.message = "The source directory changed or is missing before finalisation.";
      return out;
    }
    if (!S_ISDIR(srcIdentity->mode)) {
      out.savedErrno = ENOTDIR;
      out.message = g_strerror(ENOTDIR);
      return out;
    }

    // 1. Fast path: atomic RENAME_NOREPLACE whenever supported.
    if (callRenameNoReplace(*src, *dst) == 0) {
      out.ok = true;
      out.destinationCommitted = true;
      out.sourceRemoved = true;
      return out;
    }
    const int renameErr = errno;
    if (!isNoReplaceUnsupportedErrno(renameErr)) {
      if (isAmbiguousNetworkErrno(renameErr)) {
        const auto dstNow = queryEntryIdentityAt(dst->dirFd(), dst->leaf().c_str());
        const auto srcNow = queryEntryIdentityAt(src->dirFd(), src->leaf().c_str());
        if (dstNow && sameEntryIdentity(*dstNow, *srcIdentity) && !srcNow) {
          out.ok = true;
          out.destinationCommitted = true;
          out.sourceRemoved = true;
          return out;
        }
      }
      out.savedErrno = renameErr;
      out.message = g_strerror(renameErr);
      return out;
    }

    // 2. Approved Option A fallback for directories when RENAME_NOREPLACE is unsupported:
    //    Perform an immediate dirfd-relative existence check before ordinary renameat().
    //    The filesystem atomically rejects replacing files (ENOTDIR), symlinks (ENOTDIR),
    //    and non-empty directories (ENOTEMPTY/EEXIST).
    //    Accepted residual limitation (not equivalent to RENAME_NOREPLACE): an empty
    //    directory created concurrently between fstatat() and renameat() may be replaced.
    int dstProbeErr = 0;
    if (probeEntryAt(dst->dirFd(), dst->leaf().c_str(), dstProbeErr).has_value()) {
      out.savedErrno = EEXIST;
      out.message = g_strerror(EEXIST);
      return out;
    }
    if (dstProbeErr != ENOENT) {
      out.savedErrno = dstProbeErr;
      out.message = g_strerror(out.savedErrno);
      return out;
    }

    if (callDirectoryRename(*src, *dst) == 0) {
      out.ok = true;
      out.destinationCommitted = true;
      out.sourceRemoved = true;
      return out;
    }
    const int fallbackErr = errno;
    if (isAmbiguousNetworkErrno(fallbackErr)) {
      const auto dstNow = queryEntryIdentityAt(dst->dirFd(), dst->leaf().c_str());
      const auto srcNow = queryEntryIdentityAt(src->dirFd(), src->leaf().c_str());
      if (dstNow && sameEntryIdentity(*dstNow, *srcIdentity) && !srcNow) {
        out.ok = true;
        out.destinationCommitted = true;
        out.sourceRemoved = true;
        return out;
      }
    }
    if (fallbackErr == ENOTDIR || fallbackErr == ENOTEMPTY || fallbackErr == EEXIST) {
      out.savedErrno = EEXIST;
      out.message = g_strerror(EEXIST);
      return out;
    }
    out.savedErrno = fallbackErr;
    out.message = g_strerror(fallbackErr);
    return out;
  }

  PublishOutcome moveOrPublishEntryNoReplace(
      const char* srcPath, const char* dstPath, bool isStagedTemp, const std::optional<EntryIdentity>& expectedSource,
      bool allowSymlinkCreateFallback, const char* srcHookPath, const char* dstHookPath, int borrowedSrcDirFd,
      int borrowedDstDirFd
  ) {
    std::optional<EntryIdentity> inspected = expectedSource;
    if (!inspected) {
      if (borrowedSrcDirFd >= 0) {
        if (auto pinned = PinnedEntry::fromBorrowedDirFd(borrowedSrcDirFd, srcPath, srcHookPath))
          inspected = queryEntryIdentityAt(pinned->dirFd(), pinned->leaf().c_str());
      } else {
        inspected = queryEntryIdentity(srcPath);
      }
    }
    if (!inspected) {
      PublishOutcome out;
      out.savedErrno = errno ? errno : ENOENT;
      out.message = g_strerror(out.savedErrno);
      return out;
    }
    if (S_ISDIR(inspected->mode)) {
      return moveOrPublishDirectoryOptionA(
          srcPath, dstPath, isStagedTemp, expectedSource, srcHookPath, dstHookPath, borrowedSrcDirFd, borrowedDstDirFd
      );
    }
    return moveOrPublishNonDirectory(
        srcPath, dstPath, isStagedTemp, expectedSource, allowSymlinkCreateFallback, srcHookPath, dstHookPath,
        borrowedSrcDirFd, borrowedDstDirFd
    );
  }

  bool verifyReplaceableTarget(const char* targetPath, const std::optional<EntryIdentity>& expected, GError** error) {
    const auto current = queryEntryIdentity(targetPath);
    if (!expected || !current || !sameEntryIdentity(*current, *expected) || S_ISDIR(current->mode)) {
      g_set_error_literal(
          error, G_IO_ERROR, G_IO_ERROR_FAILED, "The destination item changed before it could be replaced."
      );
      return false;
    }
    return true;
  }

  bool allocateReplacementBackup(
      const char* targetPath, const std::optional<EntryIdentity>& expectedTarget,
      std::optional<ReplacementBackup>& backup, std::string& message, GError** error
  ) {
    backup.reset();
    if (!verifyReplaceableTarget(targetPath, expectedTarget, error))
      return false;
    int targetOpenErr = 0;
    auto targetPin = PinnedEntry::open(targetPath, targetOpenErr);
    if (!targetPin) {
      setErrorFromErrno(error, targetOpenErr ? targetOpenErr : EINVAL);
      return false;
    }
    const std::filesystem::path targetFsPath(targetPin->fullPath());
    const std::filesystem::path parent = targetFsPath.parent_path();
    if (parent.empty()) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not resolve the destination parent folder.");
      return false;
    }

    int lastUnsupportedErrno = 0;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
      g_autofree char* name = g_strdup_printf(".nythera-backup-%u", static_cast<unsigned>(g_random_int()));
      const std::filesystem::path candidatePath = parent / name;
      if (g_replaceBackupTestHook)
        g_replaceBackupTestHook(targetPath, candidatePath.c_str(), false);
      int moveErrno = 0;
      GError* moveError = nullptr;
      if (renameNoReplaceRaw(targetPath, candidatePath.c_str(), moveErrno, &moveError)) {
        if (g_replaceBackupTestHook)
          g_replaceBackupTestHook(targetPath, candidatePath.c_str(), true);
        // Post-verify the moved backup entry so a swap between the pre-check
        // and renameat2 can never cause an unapproved item to be overwritten or deleted.
        if (!verifyReplaceableTarget(candidatePath.c_str(), expectedTarget, error)) {
          ReplacementBackup provisional{
              .targetPath = targetFsPath,
              .backupPath = candidatePath,
              .expectedTarget = expectedTarget,
              .usedPrivateContainer = false,
          };
          restoreOrReportBackup(
              provisional, "The destination item changed before it could be replaced.", *error, message
          );
          return false;
        }
        backup = ReplacementBackup{
            .targetPath = targetFsPath,
            .backupPath = candidatePath,
            .expectedTarget = expectedTarget,
            .usedPrivateContainer = false,
        };
        return true;
      }
      if (isNoReplaceUnsupportedErrno(moveErrno)) {
        lastUnsupportedErrno = moveErrno;
        g_clear_error(&moveError);
        break;
      }
      if (moveErrno != EEXIST) {
        g_propagate_error(error, moveError);
        return false;
      }
      g_clear_error(&moveError);
    }

    if (lastUnsupportedErrno == 0) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "Could not allocate a safe replacement backup name.");
      return false;
    }

    // RENAME_NOREPLACE unsupported: verify destination supports safe non-overwriting
    // publication/rollback BEFORE moving the existing target into a private backup container.
    if (!S_ISLNK(expectedTarget->mode)) {
      const auto caps = probeCapabilities(parent);
      if (caps.hardLink == CapabilitySupport::Unsupported) {
        g_set_error_literal(
            error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
            "The filesystem does not support atomic non-overwriting moves (RENAME_NOREPLACE) or hard-link publication."
        );
        return false;
      }
    }

    std::string containerTemplate = (parent / ".nythera-backup-XXXXXX").string();
    if (g_mkdtemp(containerTemplate.data()) == nullptr) {
      setErrorFromErrno(error, errno);
      return false;
    }
    const std::filesystem::path containerPath(containerTemplate);
    const std::filesystem::path backupItemPath = containerPath / "item";
    const int containerFd = ::open(containerPath.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (containerFd < 0) {
      const int saved = errno;
      rmdir(containerPath.c_str());
      setErrorFromErrno(error, saved);
      return false;
    }

    if (g_replaceBackupTestHook)
      g_replaceBackupTestHook(targetPath, backupItemPath.c_str(), false);

    // containerPath was exclusively created with mode 0700 by mkdtemp, so "item" is absent.
    if (renameat(targetPin->dirFd(), targetPin->leaf().c_str(), containerFd, "item") != 0) {
      const int saved = errno;
      close(containerFd);
      rmdir(containerPath.c_str());
      setErrorFromErrno(error, saved);
      return false;
    }
    close(containerFd);

    if (g_replaceBackupTestHook)
      g_replaceBackupTestHook(targetPath, backupItemPath.c_str(), true);

    ReplacementBackup allocated{
        .targetPath = targetFsPath,
        .backupPath = backupItemPath,
        .containerPath = containerPath,
        .expectedTarget = expectedTarget,
        .usedPrivateContainer = true,
    };

    if (!verifyReplaceableTarget(backupItemPath.c_str(), expectedTarget, error)) {
      restoreOrReportBackup(allocated, "The destination item changed before it could be replaced.", *error, message);
      return false;
    }

    backup = std::move(allocated);
    return true;
  }

  void restoreOrReportBackup(
      const ReplacementBackup& backup, const char* fallbackError, GError* error, std::string& message
  ) {
    const auto outcome =
        moveOrPublishEntryNoReplace(backup.backupPath.c_str(), backup.targetPath.c_str(), false, std::nullopt, true);
    if (outcome.ok) {
      if (backup.usedPrivateContainer)
        rmdir(backup.containerPath.c_str());
      return;
    }
    message = std::string(error == nullptr ? fallbackError : error->message)
        + "; previous destination remains at "
        + backup.backupPath.string();
  }

  bool removeOrReportBackup(const ReplacementBackup& backup, const char* prefix, std::string& message) {
    GError* cleanupError = nullptr;
    if (!verifyReplaceableTarget(backup.backupPath.c_str(), backup.expectedTarget, &cleanupError)) {
      message = std::string(prefix)
          + (cleanupError == nullptr ? "unknown error" : cleanupError->message)
          + " (previous destination backup remains at "
          + backup.backupPath.string()
          + ")";
      g_clear_error(&cleanupError);
      return false;
    }
    int openErr = 0;
    auto pinned = PinnedEntry::open(backup.backupPath.c_str(), openErr);
    int unlinkErr = openErr;
    const bool unlinked = pinned
        && unlinkVerifiedAt(pinned->dirFd(), pinned->leaf().c_str(), *backup.expectedTarget, backup.backupPath.c_str(),
                            unlinkErr);
    if (!unlinked) {
      message = std::string(prefix)
          + g_strerror(unlinkErr ? unlinkErr : EIO)
          + " (previous destination backup remains at "
          + backup.backupPath.string()
          + ")";
      return false;
    }
    if (backup.usedPrivateContainer)
      rmdir(backup.containerPath.c_str());
    return true;
  }

} // namespace filemanager::fs_compat

void setFileOperationRenameNoReplaceHookForTesting(std::function<int(const char*, const char*)> hook);
void setFileOperationReplaceBackupHookForTesting(std::function<void(const char*, const char*, bool)> hook);
void setFileOperationRemoveEntryHookForTesting(std::function<int(const char*)> hook);
void setFileOperationLinkAtHookForTesting(std::function<int(const char*, const char*)> hook);
void setFileOperationSymlinkAtHookForTesting(std::function<int(const char*, const char*)> hook);
void setFileOperationDirectoryRenameHookForTesting(std::function<int(const char*, const char*)> hook);

void setFileOperationRenameNoReplaceHookForTesting(std::function<int(const char*, const char*)> hook) {
  g_renameNoReplaceTestHook = std::move(hook);
  filemanager::fs_compat::invalidateCapabilityCache();
}

void setFileOperationReplaceBackupHookForTesting(std::function<void(const char*, const char*, bool)> hook) {
  g_replaceBackupTestHook = std::move(hook);
}

void setFileOperationRemoveEntryHookForTesting(std::function<int(const char*)> hook) {
  g_removeEntryTestHook = std::move(hook);
}

void setFileOperationLinkAtHookForTesting(std::function<int(const char*, const char*)> hook) {
  g_linkAtTestHook = std::move(hook);
  filemanager::fs_compat::invalidateCapabilityCache();
}

void setFileOperationSymlinkAtHookForTesting(std::function<int(const char*, const char*)> hook) {
  g_symlinkAtTestHook = std::move(hook);
  filemanager::fs_compat::invalidateCapabilityCache();
}

void setFileOperationDirectoryRenameHookForTesting(std::function<int(const char*, const char*)> hook) {
  g_directoryRenameTestHook = std::move(hook);
  filemanager::fs_compat::invalidateCapabilityCache();
}
