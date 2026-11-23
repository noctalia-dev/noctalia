#include "filemanager/admin_helper_session.h"

#include "filemanager/file_operation_engine.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <grp.h>
#include <iterator>
#include <limits.h>
#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <linux/openat2.h>
#include <memory>
#include <mutex>
#include <poll.h>
#include <pwd.h>
#include <ranges>
#include <set>
#include <span>
#include <sys/eventfd.h>
#include <sys/fsuid.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace filemanager {
  namespace {

    struct ScopedFd {
      int fd = -1;
      explicit ScopedFd(int value = -1) noexcept : fd(value) {}
      ~ScopedFd() {
        if (fd >= 0)
          ::close(fd);
      }
      ScopedFd(const ScopedFd&) = delete;
      ScopedFd& operator=(const ScopedFd&) = delete;
      ScopedFd(ScopedFd&& other) noexcept : fd(std::exchange(other.fd, -1)) {}
      ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) {
          if (fd >= 0)
            ::close(fd);
          fd = std::exchange(other.fd, -1);
        }
        return *this;
      }
      [[nodiscard]] int release() noexcept { return std::exchange(fd, -1); }
    };

    struct ScopedDir {
      DIR* dir = nullptr;
      explicit ScopedDir(DIR* d = nullptr) noexcept : dir(d) {}
      ~ScopedDir() {
        if (dir != nullptr)
          ::closedir(dir);
      }
      ScopedDir(const ScopedDir&) = delete;
      ScopedDir& operator=(const ScopedDir&) = delete;
    };

    int openAt2NoMagiclinks(
        int dirFd, const char* path, std::uint64_t flags, std::uint64_t extraResolve = 0, std::uint64_t mode = 0
    ) noexcept {
      open_how how{};
      how.flags = flags | O_CLOEXEC;
      how.mode = mode;
      how.resolve = RESOLVE_NO_MAGICLINKS | extraResolve;
      const long rc = syscall(SYS_openat2, dirFd, path, &how, sizeof(how));
      if (rc >= 0)
        return static_cast<int>(rc);
      return -1;
    }

    AdminStatusCode statusCodeFromErrno(int err) noexcept {
      switch (err) {
      case ENOENT:
        return AdminStatusCode::NotFound;
      case ENOTDIR:
        return AdminStatusCode::NotDirectory;
      case EACCES:
      case EPERM:
        return AdminStatusCode::AccessDenied;
      case EEXIST:
        return AdminStatusCode::AlreadyExists;
      case EROFS:
        return AdminStatusCode::ReadOnlyFilesystem;
      case ENOSYS:
      case EOPNOTSUPP:
#if defined(ENOTSUP) && ENOTSUP != EOPNOTSUPP
      case ENOTSUP:
#endif
        return AdminStatusCode::NotSupported;
      case EINVAL:
      case ENAMETOOLONG:
      case ELOOP:
      case EXDEV:
        return AdminStatusCode::BadRequest;
      default:
        return AdminStatusCode::InternalError;
      }
    }

    constexpr auto kCopyChildResolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV;
    constexpr std::size_t kCopyMaxDepth = 64;
    constexpr std::size_t kCopyMaxManifestPathBytes = 32 * 1024 * 1024;

    bool supportedCopyFilesystem(int fd) {
      struct statfs fs{};
      if (::fstatfs(fd, &fs) != 0)
        return false;
      switch (fs.f_type) {
      case EXT4_SUPER_MAGIC:
      case BTRFS_SUPER_MAGIC:
      case XFS_SUPER_MAGIC:
      case F2FS_SUPER_MAGIC:
      case OVERLAYFS_SUPER_MAGIC:
      case TMPFS_MAGIC:
      case RAMFS_MAGIC:
      case MSDOS_SUPER_MAGIC:
      case EXFAT_SUPER_MAGIC:
        return true;
      default:
        return false;
      }
    }

    bool sameCopyIdentity(const struct stat& lhs, const struct stat& rhs) {
      return lhs.st_dev == rhs.st_dev && lhs.st_ino == rhs.st_ino
          && (lhs.st_mode & S_IFMT) == (rhs.st_mode & S_IFMT);
    }

    bool sameCopyRevision(const struct stat& lhs, const struct stat& rhs) {
      return sameCopyIdentity(lhs, rhs) && lhs.st_size == rhs.st_size
          && lhs.st_ctim.tv_sec == rhs.st_ctim.tv_sec && lhs.st_ctim.tv_nsec == rhs.st_ctim.tv_nsec
          && lhs.st_mtim.tv_sec == rhs.st_mtim.tv_sec && lhs.st_mtim.tv_nsec == rhs.st_mtim.tv_nsec;
    }

    // Resolve, check and open sources with the invoking account's filesystem
    // credentials. The single-threaded helper uses only the primary group.
    template <typename Open>
    ScopedFd openAsInvoker(
        uid_t uid, std::optional<gid_t> primaryGroup, Open openSource, AdminStatusCode& status, bool& credentialsRestored
    ) {
      // In-process fixtures already run with the unprivileged invoking account.
      if (::geteuid() != 0 && ::geteuid() == uid)
        return openSource();

      status = AdminStatusCode::AccessDenied;
      if (::geteuid() != 0 || uid == 0 || !primaryGroup)
        return ScopedFd{};
      __user_cap_header_struct capHeader{_LINUX_CAPABILITY_VERSION_3, 0};
      __user_cap_data_struct originalCaps[2]{};
      if (::syscall(SYS_capget, &capHeader, originalCaps) != 0)
        return ScopedFd{};
      const int groupCount = ::getgroups(0, nullptr);
      if (groupCount < 0)
        return ScopedFd{};
      std::vector<gid_t> groups(static_cast<std::size_t>(groupCount));
      if (::getgroups(groupCount, groups.data()) != groupCount || ::setgroups(0, nullptr) != 0)
        return ScopedFd{};

      const auto oldGid = static_cast<gid_t>(::setfsgid(*primaryGroup));
      const auto oldUid = static_cast<uid_t>(::setfsuid(uid));
      __user_cap_data_struct capData[2]{};
      constexpr unsigned dacCaps = (1U << CAP_DAC_OVERRIDE) | (1U << CAP_DAC_READ_SEARCH);
      ScopedFd input;
      if (static_cast<uid_t>(::setfsuid(static_cast<uid_t>(-1))) == uid
          && static_cast<gid_t>(::setfsgid(static_cast<gid_t>(-1))) == *primaryGroup
          && ::syscall(SYS_capget, &capHeader, capData) == 0
          && (capData[0].effective & dacCaps) == 0
          && ::prctl(PR_SET_DUMPABLE, 0) == 0) {
        input = openSource();
      }
      ::setfsuid(oldUid);
      ::setfsgid(oldGid);
      credentialsRestored = static_cast<uid_t>(::setfsuid(static_cast<uid_t>(-1))) == oldUid
          && static_cast<gid_t>(::setfsgid(static_cast<gid_t>(-1))) == oldGid
          && ::setgroups(groups.size(), groups.data()) == 0
          && ::syscall(SYS_capset, &capHeader, originalCaps) == 0
          && ::prctl(PR_SET_DUMPABLE, 0) == 0;
      return input;
    }

    // Directory rename has no fd-only equivalent. The wrapper must be
    // created where an untrusted user cannot exchange or remove its name.
    bool canStageCopyTree(int destination) {
      struct stat st{};
      return ::fstat(destination, &st) == 0 && st.st_uid == ::geteuid()
          && ((st.st_mode & 0022) == 0 || (st.st_mode & S_ISVTX) != 0);
    }

    bool removeCopyStageContents(int fd) {
      ScopedFd listing(openAt2NoMagiclinks(fd, ".", O_RDONLY | O_DIRECTORY, kCopyChildResolve));
      ScopedDir dir(listing.fd >= 0 ? ::fdopendir(listing.fd) : nullptr);
      if (!dir.dir)
        return false;
      (void)listing.release();
      bool ok = true;
      while (true) {
        errno = 0;
        const auto* entry = ::readdir(dir.dir);
        if (!entry)
          return errno == 0 && ok;
        const std::string_view name(entry->d_name);
        if (name == "." || name == "..")
          continue;
        struct stat st{};
        if (::fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
          ok = false;
          continue;
        }
        if (S_ISDIR(st.st_mode)) {
          ScopedFd child(openAt2NoMagiclinks(fd, entry->d_name, O_RDONLY | O_DIRECTORY, kCopyChildResolve));
          if (child.fd < 0 || !removeCopyStageContents(child.fd))
            ok = false;
          if (::unlinkat(fd, entry->d_name, AT_REMOVEDIR) != 0)
            ok = false;
        } else if (::unlinkat(fd, entry->d_name, 0) != 0) {
          ok = false;
        }
      }
    }

    struct CopyTreeStage {
      int parent = -1; // The authorised destination stays pinned for the entire request.
      ScopedFd directory;
      std::string name;

      ~CopyTreeStage() { (void)clean(); }
      bool clean() {
        if (name.empty())
          return true;
        if (directory.fd >= 0 && !removeCopyStageContents(directory.fd))
          return false;
        if (::unlinkat(parent, name.c_str(), AT_REMOVEDIR) != 0)
          return false;
        name.clear();
        directory = ScopedFd{};
        return true;
      }
    };

    using CopyDirectoryKeys = std::set<std::pair<dev_t, ino_t>>;

    // Include mount roots in the destination ancestry. Comparing the scanned
    // source identities catches bind aliases of a source or its descendants.
    std::optional<CopyDirectoryKeys> copyDestinationAncestors(int destination) {
      CopyDirectoryKeys ancestors;
      ScopedFd current(::fcntl(destination, F_DUPFD_CLOEXEC, 0));
      for (std::size_t depth = 0; current.fd >= 0 && depth < kAdminMaxPathBytes / 2; ++depth) {
        struct stat st{}, parentSt{};
        if (::fstat(current.fd, &st) != 0)
          break;
        ancestors.emplace(st.st_dev, st.st_ino);
        ScopedFd parent(openAt2NoMagiclinks(current.fd, "..", O_PATH | O_DIRECTORY));
        if (parent.fd < 0 || ::fstat(parent.fd, &parentSt) != 0)
          break;
        if (sameCopyIdentity(st, parentSt))
          return ancestors;
        current = std::move(parent);
      }
      return std::nullopt;
    }

    bool validWriteEntryName(std::string_view name) noexcept {
      return !name.empty()
          && name.size() <= NAME_MAX
          && name != "."
          && name != ".."
          && name.find('/') == std::string_view::npos
          && name.find('\0') == std::string_view::npos;
    }

    void splitStemAndExtension(std::string_view name, std::string& stemOut, std::string& extOut) {
      const std::filesystem::path p(name);
      stemOut = p.stem().string();
      extOut = p.extension().string();
    }

    class ScopedCreationUmask {
    public:
      explicit ScopedCreationUmask(const std::optional<mode_t>& umaskOverride) noexcept {
        if (umaskOverride.has_value()) {
          // Never permit group-writable or world-writable creation masks.
          m_previous = ::umask(*umaskOverride | static_cast<mode_t>(0022));
          m_active = true;
        }
      }
      ~ScopedCreationUmask() {
        if (m_active)
          ::umask(m_previous);
      }
      ScopedCreationUmask(const ScopedCreationUmask&) = delete;
      ScopedCreationUmask& operator=(const ScopedCreationUmask&) = delete;

    private:
      mode_t m_previous = 0;
      bool m_active = false;
    };

    namespace engine = file_operation_engine;
    using SessionClock = std::chrono::steady_clock;

    SessionClock::time_point sessionNow(const AdminHelperSessionConfig& config) {
      return config.clockForTesting ? config.clockForTesting() : SessionClock::now();
    }

    bool validOperationSource(engine::Kind kind, const std::string& source) {
      if (kind == engine::Kind::CreateSymlink)
        return !source.empty() && !source.starts_with("trash:");
      const std::filesystem::path path(source);
      return path.is_absolute() && path.has_filename() && path.lexically_normal().string() == source;
    }

    bool allowedMutationPath(const std::filesystem::path& path) {
      // Check the resolved parent while preserving a leaf symlink as the entry.
      std::error_code error;
      const auto parent = std::filesystem::weakly_canonical(path.parent_path(), error);
      if (error)
        return false;
      const auto resolved = parent / path.filename();
      const auto top = std::next(resolved.begin());
      return top != resolved.end() && *top != "proc" && *top != "sys" && *top != "dev";
    }

    struct OperationUpload {
      AdminRequestId id;
      std::size_t count;
      engine::Request request;
    };

    class SharedOperation {
    public:
      explicit SharedOperation(AdminRequestId id) : m_id(id), m_wakeup(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {}

      ~SharedOperation() { cancelAndJoin(); }

      void cancelAndJoin() {
        cancel();
        if (m_worker.joinable())
          m_worker.join();
      }

      AdminRequestId id() const { return m_id; }
      int fd() const { return m_wakeup.fd; }

      SessionClock::time_point lastProgress() {
        std::lock_guard lock(m_mutex);
        return m_lastProgress;
      }

      void cancel() {
        g_cancellable_cancel(m_cancel.get());
        m_pause.cancel();
        // Synchronize with the condition-variable predicate before notifying.
        {
          std::lock_guard lock(m_mutex);
        }
        m_conflictChanged.notify_all();
      }

      void setPaused(bool paused) {
        if (paused)
          m_pause.pause();
        else
          m_pause.resume();
      }

      bool answer(const AdminClientRequest& reply) {
        std::lock_guard lock(m_mutex);
        if (m_pendingConflict != reply.conflictId || m_answer || g_cancellable_is_cancelled(m_cancel.get()))
          return false;
        m_answer = engine::ConflictDecision{
            static_cast<engine::ConflictAction>(reply.conflictDecision), reply.applyToAll, reply.alternativeName
        };
        m_conflictChanged.notify_one();
        return true;
      }

      void start(engine::Request request, const AdminHelperSessionConfig& config) {
        m_worker = std::thread([this, request = std::move(request), &config]() mutable {
          const auto postResult = [&](AdminServerFrame frame) {
            post(std::move(frame));
            if (config.operationResultQueuedForTesting)
              config.operationResultQueuedForTesting();
          };
          try {
            using Kind = engine::Kind;
            if (((request.kind == Kind::Delete || request.kind == Kind::Move || request.kind == Kind::Rename)
                 && !std::ranges::all_of(request.sources, allowedMutationPath))
                || (request.kind == Kind::Rename && !allowedMutationPath(request.destination))) {
              postResult(
                  {.kind = AdminServerFrameKind::OpResult,
                   .status = AdminStatusCode::BadRequest,
                   .detail = "The operation targets a protected filesystem path."}
              );
              return;
            }
            engine::Callbacks callbacks;
            callbacks.transfer.report = [this, &config](
                                            auto bytes, auto totalBytes, auto items, auto totalItems, auto speed
                                        ) {
              const auto reportedAt = sessionNow(config);
              std::lock_guard lock(m_mutex);
              if (bytes > m_progressBytes || items > m_progressItems) {
                m_progressBytes = std::max(m_progressBytes, bytes);
                m_progressItems = std::max(m_progressItems, items);
                m_lastProgress = reportedAt;
              }
              postLocked(
                  {.kind = AdminServerFrameKind::OpProgress,
                   .completedItems = items,
                   .totalItems = totalItems,
                   .completedBytes = bytes,
                   .totalBytes = totalBytes,
                   .speed = speed}
              );
            };
            callbacks.transfer.acknowledgePause = [this](bool paused) {
              post({.kind = AdminServerFrameKind::OpPaused, .paused = paused});
            };
            callbacks.transfer.boundary = config.copyCheckpointForTesting;
            callbacks.resolveConflict = [this](
                                            const auto& source, const auto& destination, bool more,
                                            const auto& metadata, GCancellable* cancellable
                                        ) {
              std::unique_lock lock(m_mutex);
              if (g_cancellable_is_cancelled(cancellable))
                return engine::ConflictDecision{};
              m_pendingConflict = ++m_nextConflict;
              postLocked(
                  {.kind = AdminServerFrameKind::OpConflict,
                   .conflictId = m_pendingConflict,
                   .sourcePath = source,
                   .destinationPath = destination,
                   .conflictMetadata = metadata,
                   .hasRemaining = more}
              );
              m_conflictChanged.wait(lock, [&] {
                return m_answer.has_value() || g_cancellable_is_cancelled(cancellable);
              });
              m_pendingConflict = 0;
              auto answer = std::exchange(m_answer, std::nullopt);
              return g_cancellable_is_cancelled(cancellable) ? engine::ConflictDecision{}
                                                             : answer.value_or(engine::ConflictDecision{});
            };
            engine::Result execution;
            {
              ScopedCreationUmask umaskGuard(config.creationUmask);
              execution = engine::executeLocalOperation(request, m_cancel.get(), &m_pause, callbacks);
            }
            for (auto& entry : execution.completedEntries)
              post({.kind = AdminServerFrameKind::OpCompletedEntry, .completedEntry = std::move(entry)});
            for (auto& path : execution.movedSources)
              post({.kind = AdminServerFrameKind::OpMovedSource, .sourcePath = std::move(path)});
            postResult(
                {.kind = AdminServerFrameKind::OpResult,
                 .status = execution.ok    ? AdminStatusCode::Ok
                     : execution.cancelled ? AdminStatusCode::Cancelled
                                           : AdminStatusCode::InternalError,
                 .detail = std::move(execution.message),
                 .completedItems = execution.completedMeasure.items,
                 .totalItems = execution.totalMeasure.items,
                 .completedBytes = execution.completedMeasure.bytes,
                 .totalBytes = execution.totalMeasure.bytes,
                 .destinationPath = std::move(request.destination),
                 .mutated = execution.mutated,
                 .customDescription = std::move(execution.customDescription)}
            );
          } catch (...) {
            postResult(
                {.kind = AdminServerFrameKind::OpResult,
                 .status = AdminStatusCode::InternalError,
                 .detail = "The shared filesystem operation could not be completed."}
            );
          }
        });
      }

      std::deque<AdminServerFrame> takeFrames() {
        std::uint64_t value = 0;
        while (::read(m_wakeup.fd, &value, sizeof(value)) < 0 && errno == EINTR) {
        }
        std::lock_guard lock(m_mutex);
        return std::exchange(m_frames, {});
      }

    private:
      void post(AdminServerFrame frame) {
        std::lock_guard lock(m_mutex);
        postLocked(std::move(frame));
      }

      void postLocked(AdminServerFrame frame) {
        frame.requestId = m_id;
        if (!m_frames.empty() && frame.kind == AdminServerFrameKind::OpProgress && m_frames.back().kind == frame.kind)
          m_frames.back() = std::move(frame);
        else
          m_frames.push_back(std::move(frame));
        const std::uint64_t value = 1;
        while (::write(m_wakeup.fd, &value, sizeof(value)) < 0 && errno == EINTR) {
        }
      }

      AdminRequestId m_id;
      ScopedFd m_wakeup;
      std::unique_ptr<GCancellable, decltype(&g_object_unref)> m_cancel{g_cancellable_new(), g_object_unref};
      engine::FileTransferPauseState m_pause;
      std::mutex m_mutex;
      std::condition_variable m_conflictChanged;
      std::uint64_t m_nextConflict = 0;
      std::uint64_t m_pendingConflict = 0;
      std::optional<engine::ConflictDecision> m_answer;
      SessionClock::time_point m_lastProgress = SessionClock::time_point::min();
      std::uint64_t m_progressBytes = 0;
      std::uint64_t m_progressItems = 0;
      std::deque<AdminServerFrame> m_frames;
      std::thread m_worker;
    };

    std::string specialModeContentType(mode_t mode) {
      if (S_ISDIR(mode))
        return "inode/directory";
      if (S_ISFIFO(mode))
        return "inode/fifo";
      if (S_ISSOCK(mode))
        return "inode/socket";
      if (S_ISCHR(mode))
        return "inode/chardevice";
      if (S_ISBLK(mode))
        return "inode/blockdevice";
      return {};
    }

    void readPinnedSymlinkTarget(int linkFd, const std::filesystem::path& parentDir, std::string& out) {
      std::array<char, PATH_MAX> buf{};
      const ssize_t len = ::readlinkat(linkFd, "", buf.data(), buf.size());
      if (len < 0 || static_cast<std::size_t>(len) == buf.size())
        return;
      const std::filesystem::path rawPath(std::string_view(buf.data(), static_cast<std::size_t>(len)));
      auto target = (rawPath.is_absolute() ? rawPath : parentDir / rawPath).lexically_normal().string();
      if (target.size() <= kAdminMaxPathBytes)
        out = std::move(target);
    }

    struct EntryMetadata {
      mode_t mode = 0;
      std::uint64_t size = 0;
      std::uint64_t device = 0;
      std::uint64_t inode = 0;
      std::uint64_t mtimeSec = 0;
      bool hasCreated = false;
      std::uint64_t createdSec = 0;
      bool hasAccessed = false;
      std::uint64_t accessedSec = 0;
    };

    bool queryEntryMetadataAt(int dirFd, const char* name, int extraFlags, EntryMetadata& out) noexcept {
      out = {};
      struct statx stx{};
      if (statx(
              dirFd, name, AT_SYMLINK_NOFOLLOW | AT_STATX_SYNC_AS_STAT | extraFlags,
              STATX_BASIC_STATS | STATX_BTIME, &stx
          )
          == 0) {
        out.mode = stx.stx_mode;
        out.size = stx.stx_size;
        out.device = static_cast<std::uint64_t>(makedev(stx.stx_dev_major, stx.stx_dev_minor));
        out.inode = stx.stx_ino;
        out.mtimeSec = stx.stx_mtime.tv_sec >= 0 ? static_cast<std::uint64_t>(stx.stx_mtime.tv_sec) : 0ULL;
        if ((stx.stx_mask & STATX_BTIME) != 0 && stx.stx_btime.tv_sec > 0) {
          out.hasCreated = true;
          out.createdSec = static_cast<std::uint64_t>(stx.stx_btime.tv_sec);
        }
        if ((stx.stx_mask & STATX_ATIME) != 0 && stx.stx_atime.tv_sec > 0) {
          out.hasAccessed = true;
          out.accessedSec = static_cast<std::uint64_t>(stx.stx_atime.tv_sec);
        }
        return true;
      }
      struct stat st{};
      if (fstatat(dirFd, name, &st, AT_SYMLINK_NOFOLLOW | extraFlags) != 0)
        return false;
      out.mode = st.st_mode;
      out.size = st.st_size >= 0 ? static_cast<std::uint64_t>(st.st_size) : 0ULL;
      out.device = static_cast<std::uint64_t>(st.st_dev);
      out.inode = static_cast<std::uint64_t>(st.st_ino);
      out.mtimeSec = st.st_mtim.tv_sec >= 0 ? static_cast<std::uint64_t>(st.st_mtim.tv_sec) : 0ULL;
      if (st.st_atim.tv_sec > 0) {
        out.hasAccessed = true;
        out.accessedSec = static_cast<std::uint64_t>(st.st_atim.tv_sec);
      }
      return true;
    }

    bool queryDirectoryWritableFd(int dirFd) noexcept {
      struct statvfs vfs{};
      if (fstatvfs(dirFd, &vfs) == 0 && (vfs.f_flag & ST_RDONLY) != 0)
        return false;
      return faccessat(dirFd, "", W_OK | X_OK, AT_EMPTY_PATH | AT_EACCESS) == 0;
    }

    bool queryDirectoryReadableFd(int dirFd) noexcept {
      return faccessat(dirFd, "", R_OK | X_OK, AT_EMPTY_PATH | AT_EACCESS) == 0;
    }

    bool closeExtraFileDescriptors() noexcept {
#ifdef SYS_close_range
      if (syscall(SYS_close_range, STDERR_FILENO + 1, ~0U, 0U) == 0)
        return true;
#endif
      // A descriptor may be above the current soft limit if that limit was lowered after opening it.
      const ScopedDir descriptors(::opendir("/proc/self/fd"));
      if (descriptors.dir == nullptr)
        return false;
      const int enumerationFd = ::dirfd(descriptors.dir);
      while (true) {
        errno = 0;
        const auto* entry = ::readdir(descriptors.dir);
        if (!entry)
          return errno == 0;
        const std::string_view name(entry->d_name);
        int fd = -1;
        const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), fd);
        if (error == std::errc{} && end == name.data() + name.size() && fd > STDERR_FILENO && fd != enumerationFd)
          ::close(fd);
      }
    }

    int clampNegotiatedInactivityTimeoutMs(
        const std::optional<int>& configOverrideMs, std::uint32_t clientRequestedMs
    ) noexcept {
      if (configOverrideMs.has_value()) {
        return std::clamp(
            *configOverrideMs, kMinAdminHelperInactivityTimeoutMs, kMaxAdminHelperInactivityTimeoutMs
        );
      }
      if (clientRequestedMs > 0) {
        const auto clamped = std::clamp<std::uint64_t>(
            clientRequestedMs,
            static_cast<std::uint64_t>(kMinAdminInactivityTimeoutSeconds * 1000),
            static_cast<std::uint64_t>(kMaxAdminHelperInactivityTimeoutMs)
        );
        return static_cast<int>(clamped);
      }
      return kDefaultAdminHelperInactivityTimeoutMs;
    }

  } // namespace

  bool validateAdminHelperInvocation(
      int argc, uid_t effectiveUid, const char* pkexecUidEnv, std::uint32_t& invokingUidOut, std::string& errorOut
  ) {
    invokingUidOut = 0;
    if (argc != 1) {
      errorOut = "Unexpected command-line arguments.";
      return false;
    }
    if (effectiveUid != 0 || pkexecUidEnv == nullptr || pkexecUidEnv[0] == '\0') {
      errorOut = "Run only through the installed Polkit action.";
      return false;
    }
    const std::string_view rawUid(pkexecUidEnv);
    if (rawUid.front() == '+' || rawUid.front() == '-') {
      errorOut = "Invalid PKEXEC_UID environment variable.";
      return false;
    }
    std::uint32_t parsedUid = 0;
    const auto parsed = std::from_chars(rawUid.data(), rawUid.data() + rawUid.size(), parsedUid, 10);
    if (parsed.ec != std::errc{}
        || parsed.ptr != rawUid.data() + rawUid.size()
        || parsedUid == 0
        || parsedUid == static_cast<std::uint32_t>(-1)) {
      errorOut = "Invalid invoking user ID.";
      return false;
    }
    invokingUidOut = parsedUid;
    return true;
  }

  bool applyAdminHelperProcessHardening(std::string& errorOut) {
    umask(0077);
    std::signal(SIGPIPE, SIG_IGN);
    if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0 || prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
      errorOut = "Could not restrict the Administrator helper process.";
      return false;
    }
    if (!closeExtraFileDescriptors()) {
      errorOut = "Could not close inherited helper descriptors.";
      return false;
    }
    if (chdir("/") != 0) {
      errorOut = "Could not switch working directory to root.";
      return false;
    }
    if (clearenv() != 0
        || setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1) != 0
        || setenv("LC_ALL", "C", 1) != 0
        || setenv("GIO_USE_VFS", "local", 1) != 0
        || setenv("GVFS_DISABLE_FUSE", "1", 1) != 0) {
      errorOut = "Could not sanitize helper process environment.";
      return false;
    }
    return true;
  }

  AdminHelperSessionResult runAdminHelperSession(int inFd, int outFd, const AdminHelperSessionConfig& config) {
    AdminHelperSessionResult result;
    if (inFd < 0 || outFd < 0) {
      result.reason = AdminHelperSessionExitReason::IoError;
      result.detail = "Invalid communication file descriptors.";
      return result;
    }

    for (const int fd : {inFd, outFd}) {
      const int flags = fcntl(fd, F_GETFL);
      if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        result.reason = AdminHelperSessionExitReason::IoError;
        result.detail = "Could not configure non-blocking communication streams.";
        return result;
      }
    }

    const std::uint32_t helperPid =
        config.helperPid != 0 ? config.helperPid : static_cast<std::uint32_t>(getpid());
    const auto sessionStart = sessionNow(config);
    auto lastActivity = sessionStart;
    int activeInactivityTimeoutMs =
        clampNegotiatedInactivityTimeoutMs(config.inactivityTimeoutMs, 0);
    result.negotiatedInactivityTimeoutMs = activeInactivityTimeoutMs;

    AdminLineBuffer lineBuffer(config.maxFrameBytes);
    std::size_t consecutiveBadRequests = 0;
    std::optional<OperationUpload> upload;
    std::unique_ptr<SharedOperation> sharedOperation;

    auto cancelShared = [&](AdminRequestId id) {
      if (sharedOperation && sharedOperation->id() == id)
        sharedOperation->cancel();
    };

    auto remainingTimeMs = [&]() {
      if (config.terminate && *config.terminate != 0) {
        result.reason = AdminHelperSessionExitReason::CleanShutdown;
        result.detail = "Administrator helper was terminated.";
        return 0;
      }
      const int timeout = result.handshakeCompleted ? activeInactivityTimeoutMs : config.handshakeTimeoutMs;
      if (timeout < 0)
        return -1;
      const auto now = sessionNow(config);
      if (sharedOperation)
        lastActivity = std::max(lastActivity, sharedOperation->lastProgress());
      const auto from = result.handshakeCompleted ? lastActivity : sessionStart;
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          now - from
      ).count();
      if (elapsed < timeout)
        return static_cast<int>(timeout - std::max<std::int64_t>(0, elapsed));
      result.reason = result.handshakeCompleted ? AdminHelperSessionExitReason::InactivityTimeout
                                                : AdminHelperSessionExitReason::HandshakeTimeout;
      result.detail = result.handshakeCompleted ? "Session terminated due to inactivity timeout."
                                                : "Timed out waiting for HELLO handshake.";
      return 0;
    };

    constexpr auto sharedWriteTimeout = std::chrono::seconds(5);
    auto sendServerFrame = [&](
                               int fd, const AdminServerFrame& frame, bool committed = false,
                               std::optional<SessionClock::time_point> writeDeadline = {}
                           ) {
      const auto encoded = encodeAdminServerFrame(frame);
      if (!encoded)
        return false;
      // Shared frames must remain writable during expiry, including a partially written frame.
      // Bound IPC backpressure separately from the session's idle deadline.
      if (!writeDeadline && sharedOperation)
        writeDeadline = SessionClock::now() + sharedWriteTimeout;
      std::string_view remaining(*encoded);
      while (!remaining.empty()) {
        const int timeout = writeDeadline ? static_cast<int>(std::max(
                                 std::chrono::milliseconds::zero(),
                                 std::chrono::ceil<std::chrono::milliseconds>(*writeDeadline - SessionClock::now())
                             ).count())
                                          : remainingTimeMs();
        // An already committed COPY_FILE may report success with a nonblocking
        // write even if the deadline elapsed in linkat. Never wait past expiry.
        if (timeout == 0 && !committed)
          return false;
        const ssize_t n = ::write(fd, remaining.data(), remaining.size());
        if (n > 0) {
          remaining.remove_prefix(static_cast<std::size_t>(n));
          continue;
        }
        if (n < 0 && errno == EINTR)
          continue;
        if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
          return false;
        if (timeout == 0)
          return false;
        // A client can close its request stream while it is still reading terminal replies.
        pollfd fds[]{{fd, POLLOUT, 0}, {writeDeadline ? -1 : inFd, 0, 0}};
        const int rc = ::poll(fds, 2, timeout);
        if (rc < 0 && errno == EINTR)
          continue;
        if (rc <= 0
            || (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0
            || (fds[1].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0)
          return false;
      }
      return true;
    };

    auto sendResponseFrame = [&](int fd, AdminRequestId requestId, AdminStatusCode status, std::string_view detail) {
      if (requestId == kInvalidAdminRequestId)
        return true;
      return sendServerFrame(fd, {
          .kind = AdminServerFrameKind::Response, .requestId = requestId, .status = status,
          .detail = std::string(detail),
      });
    };

    auto flushShared = [&](bool expiring = false) {
      if (sharedOperation) {
        const auto writeDeadline = SessionClock::now() + sharedWriteTimeout;
        for (auto& frame : sharedOperation->takeFrames()) {
          if (expiring && frame.kind == AdminServerFrameKind::OpResult && frame.status == AdminStatusCode::Cancelled)
            frame.detail = frame.detail.empty() ? result.detail : result.detail + " " + frame.detail;
          if (!sendServerFrame(outFd, frame, false, writeDeadline)) {
            if (expiring)
              result.detail += " The final operation outcome could not be delivered.";
            else {
              result.reason = AdminHelperSessionExitReason::IoError;
              result.detail = "Failed to write shared operation frame.";
            }
            return false;
          }
          if (frame.kind == AdminServerFrameKind::OpResult) {
            lastActivity = std::max(lastActivity, sharedOperation->lastProgress());
            sharedOperation.reset();
          }
        }
      }

      return true;
    };

    auto drainExpiredOperation = [&] {
      if (result.reason == AdminHelperSessionExitReason::InactivityTimeout && sharedOperation) {
        sharedOperation->cancelAndJoin();
        (void)flushShared(true);
      }
    };

    auto sharedControl = [&](const AdminClientRequest& request) {
      const bool active = sharedOperation && sharedOperation->id() == request.targetRequestId;
      if (!active)
        return sendResponseFrame(
            outFd, request.requestId, AdminStatusCode::BadRequest, "No matching shared operation."
        );
      if (request.command == AdminClientCommand::OpConflictReply) {
        if (!sharedOperation->answer(request))
          return sendResponseFrame(
              outFd, request.requestId, AdminStatusCode::BadRequest, "No matching pending conflict."
          );
      } else {
        sharedOperation->setPaused(request.command == AdminClientCommand::OpPause);
      }
      lastActivity = sessionNow(config);
      return sendResponseFrame(outFd, request.requestId, AdminStatusCode::Ok, {});
    };

    // Cancellation can overtake queued requests during enumeration. Keep the queue bounded
    // and process all other requests in their original order after the enumeration ends.
    constexpr std::size_t maxDeferredRequests = 64;
    std::deque<std::string> deferredLines;
    auto checkStreamingCancelled = [&](AdminRequestId activeReqId, bool& shouldExit, bool checkDeferred = false) {
      if (remainingTimeMs() == 0 || !flushShared()) {
        shouldExit = true;
        return true;
      }
      if (checkDeferred && !deferredLines.empty()) {
        const auto cancel = std::ranges::find_if(deferredLines, [&](const std::string& line) {
          AdminClientRequest incoming;
          return decodeAdminClientRequest(line, incoming)
              && incoming.command == AdminClientCommand::Cancel
              && incoming.targetRequestId == activeReqId;
        });
        if (cancel != deferredLines.end()) {
          AdminClientRequest incoming;
          (void)decodeAdminClientRequest(*cancel, incoming);
          deferredLines.erase(cancel);
          result.lastCancelledTargetId = activeReqId;
          ++result.requestsHandled;
          (void)sendResponseFrame(outFd, incoming.requestId, AdminStatusCode::Ok, "Cancellation acknowledged.");
          return true;
        }
      }
      std::array<char, 4096> chunk{};
      const ssize_t n = ::read(inFd, chunk.data(), chunk.size());
      if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
        result.reason = n == 0 ? AdminHelperSessionExitReason::ClientEof : AdminHelperSessionExitReason::IoError;
        result.detail = "Client channel closed during streaming request.";
        shouldExit = true;
        return true;
      }
      if (n > 0)
        lineBuffer.append(std::string_view(chunk.data(), static_cast<std::size_t>(n)));

      std::string queuedLine;
      while (true) {
        const auto status = lineBuffer.popFrame(queuedLine);
        if (status == AdminFrameReadStatus::NeedMoreData)
          return false;
        if (status == AdminFrameReadStatus::OversizedFrame || status == AdminFrameReadStatus::MalformedFrame) {
          result.reason = AdminHelperSessionExitReason::ProtocolViolation;
          result.detail = "Invalid protocol frame during streaming request.";
          shouldExit = true;
          return true;
        }
        AdminClientRequest incoming;
        if (decodeAdminClientRequest(queuedLine, incoming)) {
          if (incoming.command == AdminClientCommand::Shutdown) {
            if (sharedOperation) {
              sharedOperation->cancelAndJoin();
              (void)flushShared();
            }
            ++result.requestsHandled;
            (void)sendResponseFrame(
                outFd, incoming.requestId, AdminStatusCode::ShuttingDown, "Helper session terminated."
            );
            result.reason = AdminHelperSessionExitReason::CleanShutdown;
            shouldExit = true;
            return true;
          }
          if (incoming.command == AdminClientCommand::Cancel
              && sharedOperation
              && incoming.targetRequestId == sharedOperation->id()) {
            cancelShared(incoming.targetRequestId);
            result.lastCancelledTargetId = incoming.targetRequestId;
            ++result.requestsHandled;
            (void)sendResponseFrame(outFd, incoming.requestId, AdminStatusCode::Ok, "Cancellation acknowledged.");
            continue;
          }
          if (incoming.command == AdminClientCommand::OpPause
              || incoming.command == AdminClientCommand::OpResume
              || incoming.command == AdminClientCommand::OpConflictReply) {
            ++result.requestsHandled;
            if (!sharedControl(incoming)) {
              result.reason = AdminHelperSessionExitReason::IoError;
              result.detail = "Failed to write shared operation control response.";
              shouldExit = true;
              return true;
            }
            continue;
          }
          if (incoming.command == AdminClientCommand::Cancel && incoming.targetRequestId == activeReqId) {
            result.lastCancelledTargetId = incoming.targetRequestId;
            ++result.requestsHandled;
            (void)sendResponseFrame(outFd, incoming.requestId, AdminStatusCode::Ok, "Cancellation acknowledged.");
            return true;
          }
        }
        if (deferredLines.size() == maxDeferredRequests) {
          result.reason = AdminHelperSessionExitReason::ProtocolViolation;
          result.detail = "Too many pipelined requests during streaming request.";
          shouldExit = true;
          return true;
        }
        deferredLines.push_back(std::move(queuedLine));
      }
    };

    struct AuthorizedDirectory {
      std::string path;
      ScopedFd fd;
      std::uint64_t device = 0;
      std::uint64_t inode = 0;
      uid_t ownerUid = 0;
      gid_t ownerGid = 0;
      AdminRequestId listingId = kInvalidAdminRequestId;

      void reset() noexcept {
        path.clear();
        fd = ScopedFd{};
        device = 0;
        inode = 0;
        ownerUid = 0;
        ownerGid = 0;
        listingId = kInvalidAdminRequestId;
      }
    };
    AuthorizedDirectory authorizedDir;

    auto executeListDir = [&](const AdminClientRequest& request, bool& shouldExit) -> bool {
      if (request.path != authorizedDir.path)
        authorizedDir.reset();
      const int rawFd = openAt2NoMagiclinks(AT_FDCWD, request.path.c_str(), O_RDONLY | O_DIRECTORY | O_NOATIME);
      if (rawFd < 0) {
        const int err = errno;
        authorizedDir.reset();
        return sendResponseFrame(outFd, request.requestId, statusCodeFromErrno(err), std::strerror(err));
      }
      struct stat rawSt{};
      if (::fstat(rawFd, &rawSt) != 0 || !S_ISDIR(rawSt.st_mode)) {
        const int err = errno != 0 ? errno : ENOTDIR;
        ::close(rawFd);
        authorizedDir.reset();
        return sendResponseFrame(outFd, request.requestId, statusCodeFromErrno(err), std::strerror(err));
      }
      ScopedFd pinnedFd(
          openAt2NoMagiclinks(rawFd, ".", O_PATH | O_DIRECTORY, RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS)
      );
      struct stat pinnedSt{};
      if (pinnedFd.fd < 0
          || ::fstat(pinnedFd.fd, &pinnedSt) != 0
          || !S_ISDIR(pinnedSt.st_mode)
          || pinnedSt.st_dev != rawSt.st_dev
          || pinnedSt.st_ino != rawSt.st_ino) {
        const int err = errno != 0 ? errno : EIO;
        ::close(rawFd);
        authorizedDir.reset();
        return sendResponseFrame(outFd, request.requestId, statusCodeFromErrno(err), std::strerror(err));
      }
      const bool writable = queryDirectoryWritableFd(rawFd);
      DIR* rawDir = ::fdopendir(rawFd);
      if (rawDir == nullptr) {
        const int err = errno;
        ::close(rawFd);
        authorizedDir.reset();
        return sendResponseFrame(outFd, request.requestId, statusCodeFromErrno(err), std::strerror(err));
      }
      ScopedDir dir(rawDir);
      const int dirFd = ::dirfd(dir.dir);
      // 32 records fit even at the protocol's maximum encoded name and metadata lengths.
      const auto batchLimit = std::min(request.maxBatchEntries, kAdminDefaultDirBatchEntries);

      AdminServerFrame batchFrame;
      batchFrame.kind = AdminServerFrameKind::DirBatch;
      batchFrame.requestId = request.requestId;
      batchFrame.entries.reserve(batchLimit);

      std::uint64_t totalEntries = 0;
      std::uint64_t scannedEntries = 0;
      while (true) {
        errno = 0;
        dirent* child = ::readdir(dir.dir);
        if (child == nullptr) {
          if (errno != 0) {
            const int err = errno;
            authorizedDir.reset();
            return sendResponseFrame(outFd, request.requestId, statusCodeFromErrno(err), std::strerror(err));
          }
          break;
        }
        if (std::strcmp(child->d_name, ".") == 0 || std::strcmp(child->d_name, "..") == 0)
          continue;
        if (scannedEntries >= kAdminMaxDirectoryEntries)
          return sendResponseFrame(outFd, request.requestId, AdminStatusCode::InternalError, "Directory entry limit exceeded.");
        if ((scannedEntries++ % batchLimit) == 0U) {
          if (checkStreamingCancelled(request.requestId, shouldExit)) {
            return !shouldExit && sendResponseFrame(
                outFd, request.requestId, AdminStatusCode::Cancelled, "Directory enumeration cancelled."
            );
          }
        }

        EntryMetadata meta;
        if (!queryEntryMetadataAt(dirFd, child->d_name, 0, meta))
          continue;

        AdminDirEntryRecord rec;
        rec.name = child->d_name;
        rec.mtimeSec = meta.mtimeSec;
        rec.hasCreated = meta.hasCreated;
        rec.createdSec = meta.createdSec;
        rec.hasAccessed = meta.hasAccessed;
        rec.accessedSec = meta.accessedSec;

        if (S_ISLNK(meta.mode)) {
          rec.isSymlink = true;
          rec.device = meta.device;
          rec.inode = meta.inode;
          rec.hasIdentity = true;
          struct stat targetSt{};
          const ScopedFd targetFd(openAt2NoMagiclinks(dirFd, child->d_name, O_PATH));
          if (targetFd.fd >= 0 && ::fstat(targetFd.fd, &targetSt) == 0) {
            rec.brokenSymlink = false;
            rec.isDir = S_ISDIR(targetSt.st_mode);
            rec.size = S_ISREG(targetSt.st_mode) && targetSt.st_size >= 0
                ? static_cast<std::uint64_t>(targetSt.st_size)
                : meta.size;
            rec.contentType = specialModeContentType(targetSt.st_mode);
          } else {
            rec.brokenSymlink = true;
            rec.isDir = false;
            rec.size = meta.size;
          }
        } else {
          rec.isSymlink = false;
          rec.brokenSymlink = false;
          rec.isDir = S_ISDIR(meta.mode);
          rec.size = rec.isDir ? 0ULL : meta.size;
          rec.device = meta.device;
          rec.inode = meta.inode;
          rec.hasIdentity = true;
          rec.contentType = specialModeContentType(meta.mode);
        }

        batchFrame.entries.push_back(std::move(rec));
        ++totalEntries;

        if (batchFrame.entries.size() >= batchLimit) {
          if (!sendServerFrame(outFd, batchFrame)) {
            result.reason = AdminHelperSessionExitReason::IoError;
            result.detail = "Failed to write DIR_BATCH frame.";
            shouldExit = true;
            return false;
          }
          batchFrame.entries.clear();
        }
      }

      if (!batchFrame.entries.empty()) {
        if (!sendServerFrame(outFd, batchFrame)) {
          result.reason = AdminHelperSessionExitReason::IoError;
          result.detail = "Failed to write final DIR_BATCH frame.";
          shouldExit = true;
          return false;
        }
      }

      AdminServerFrame endFrame;
      endFrame.kind = AdminServerFrameKind::DirEnd;
      endFrame.requestId = request.requestId;
      endFrame.directoryWritable = writable;
      endFrame.totalEntries = totalEntries;
      if (!sendServerFrame(outFd, endFrame)) {
        result.reason = AdminHelperSessionExitReason::IoError;
        result.detail = "Failed to write DIR_END frame.";
        shouldExit = true;
        return false;
      }

      authorizedDir.path = request.path;
      authorizedDir.fd = std::move(pinnedFd);
      authorizedDir.device = static_cast<std::uint64_t>(pinnedSt.st_dev);
      authorizedDir.inode = static_cast<std::uint64_t>(pinnedSt.st_ino);
      authorizedDir.ownerUid = pinnedSt.st_uid;
      authorizedDir.ownerGid = pinnedSt.st_gid;
      authorizedDir.listingId = request.requestId;
      return true;
    };

    auto executeStatInfo = [&](const AdminClientRequest& request) -> bool {
      const std::filesystem::path targetPath(request.path);
      AdminServerFrame frame;
      frame.kind = AdminServerFrameKind::StatResult;
      frame.requestId = request.requestId;

      if (targetPath == targetPath.root_path()) {
        ScopedFd rootFd(openAt2NoMagiclinks(AT_FDCWD, targetPath.c_str(), O_PATH));
        EntryMetadata meta;
        if (rootFd.fd < 0 || !queryEntryMetadataAt(rootFd.fd, "", AT_EMPTY_PATH, meta)) {
          frame.statInfo.exists = false;
          return sendServerFrame(outFd, frame);
        }
        frame.statInfo.exists = true;
        frame.statInfo.isDir = S_ISDIR(meta.mode);
        frame.statInfo.isSymlink = false;
        frame.statInfo.brokenSymlink = false;
        frame.statInfo.writable = queryDirectoryWritableFd(rootFd.fd);
        frame.statInfo.readable = queryDirectoryReadableFd(rootFd.fd);
        frame.statInfo.hasSize = false;
        frame.statInfo.device = meta.device;
        frame.statInfo.inode = meta.inode;
        frame.statInfo.mtimeSec = meta.mtimeSec;
        frame.statInfo.hasCreated = meta.hasCreated;
        frame.statInfo.createdSec = meta.createdSec;
        frame.statInfo.hasAccessed = meta.hasAccessed;
        frame.statInfo.accessedSec = meta.accessedSec;
        frame.statInfo.contentType = "inode/directory";
        return sendServerFrame(outFd, frame);
      }

      const std::filesystem::path parentDir = targetPath.parent_path();
      const std::string leaf = targetPath.filename().string();
      ScopedFd parentFd(openAt2NoMagiclinks(AT_FDCWD, parentDir.c_str(), O_PATH | O_DIRECTORY));
      const ScopedFd leafFd(parentFd.fd < 0 ? -1 : openAt2NoMagiclinks(
          parentFd.fd, leaf.c_str(), O_PATH | O_NOFOLLOW, RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS
      ));
      EntryMetadata meta;
      if (leafFd.fd < 0 || !queryEntryMetadataAt(leafFd.fd, "", AT_EMPTY_PATH, meta)) {
        frame.statInfo.exists = false;
        return sendServerFrame(outFd, frame);
      }

      frame.statInfo.exists = true;
      frame.statInfo.device = meta.device;
      frame.statInfo.inode = meta.inode;
      frame.statInfo.mtimeSec = meta.mtimeSec;
      frame.statInfo.hasCreated = meta.hasCreated;
      frame.statInfo.createdSec = meta.createdSec;
      frame.statInfo.hasAccessed = meta.hasAccessed;
      frame.statInfo.accessedSec = meta.accessedSec;

      if (S_ISLNK(meta.mode)) {
        frame.statInfo.isSymlink = true;
        frame.statInfo.hasSize = true;
        frame.statInfo.size = meta.size;
        readPinnedSymlinkTarget(leafFd.fd, parentDir, frame.statInfo.symlinkTarget);
        struct stat targetSt{};
        const ScopedFd targetFd(openAt2NoMagiclinks(parentFd.fd, leaf.c_str(), O_PATH));
        if (targetFd.fd >= 0 && ::fstat(targetFd.fd, &targetSt) == 0) {
          frame.statInfo.brokenSymlink = false;
          frame.statInfo.isDir = S_ISDIR(targetSt.st_mode);
          frame.statInfo.contentType = specialModeContentType(targetSt.st_mode);
          if (frame.statInfo.isDir) {
            frame.statInfo.writable = queryDirectoryWritableFd(targetFd.fd);
            frame.statInfo.readable = queryDirectoryReadableFd(targetFd.fd);
          }
        } else {
          frame.statInfo.brokenSymlink = true;
          frame.statInfo.isDir = false;
        }
      } else {
        frame.statInfo.isSymlink = false;
        frame.statInfo.brokenSymlink = false;
        frame.statInfo.isDir = S_ISDIR(meta.mode);
        frame.statInfo.hasSize = S_ISREG(meta.mode);
        frame.statInfo.size = frame.statInfo.hasSize ? meta.size : 0ULL;
        frame.statInfo.contentType = specialModeContentType(meta.mode);
        if (frame.statInfo.isDir) {
          frame.statInfo.writable = queryDirectoryWritableFd(leafFd.fd);
          frame.statInfo.readable = queryDirectoryReadableFd(leafFd.fd);
        } else {
          frame.statInfo.readable = ::faccessat(leafFd.fd, "", R_OK, AT_EMPTY_PATH | AT_EACCESS) == 0;
          frame.statInfo.writable = ::faccessat(leafFd.fd, "", W_OK, AT_EMPTY_PATH | AT_EACCESS) == 0;
        }
      }
      return sendServerFrame(outFd, frame);
    };

    auto executeStatVfs = [&](const AdminClientRequest& request) -> bool {
      ScopedFd fd(openAt2NoMagiclinks(AT_FDCWD, request.path.c_str(), O_PATH));
      struct statvfs vfs{};
      if (fd.fd < 0 || ::fstatvfs(fd.fd, &vfs) != 0) {
        const int err = errno != 0 ? errno : EIO;
        return sendResponseFrame(outFd, request.requestId, statusCodeFromErrno(err), std::strerror(err));
      }
      const std::uint64_t unit = vfs.f_frsize != 0 ? static_cast<std::uint64_t>(vfs.f_frsize)
                                                   : static_cast<std::uint64_t>(vfs.f_bsize);
      AdminServerFrame frame;
      frame.kind = AdminServerFrameKind::StatVfsResult;
      frame.requestId = request.requestId;
      frame.statVfsInfo.totalBytes = static_cast<std::uint64_t>(vfs.f_blocks) * unit;
      frame.statVfsInfo.freeBytes = static_cast<std::uint64_t>(vfs.f_bfree) * unit;
      frame.statVfsInfo.availableBytes = static_cast<std::uint64_t>(vfs.f_bavail) * unit;
      frame.statVfsInfo.readOnly = (vfs.f_flag & ST_RDONLY) != 0;
      return sendServerFrame(outFd, frame);
    };

    auto executeCountDir = [&](const AdminClientRequest& request, bool& shouldExit) -> bool {
      const int rawFd = openAt2NoMagiclinks(AT_FDCWD, request.path.c_str(), O_RDONLY | O_DIRECTORY | O_NOATIME);
      if (rawFd < 0) {
        const int err = errno;
        return sendResponseFrame(outFd, request.requestId, statusCodeFromErrno(err), std::strerror(err));
      }
      DIR* rawDir = ::fdopendir(rawFd);
      if (rawDir == nullptr) {
        const int err = errno;
        ::close(rawFd);
        return sendResponseFrame(outFd, request.requestId, statusCodeFromErrno(err), std::strerror(err));
      }
      ScopedDir dir(rawDir);

      std::uint64_t count = 0;
      while (true) {
        errno = 0;
        dirent* child = ::readdir(dir.dir);
        if (child == nullptr) {
          if (errno != 0) {
            const int err = errno;
            return sendResponseFrame(outFd, request.requestId, statusCodeFromErrno(err), std::strerror(err));
          }
          break;
        }
        if (std::strcmp(child->d_name, ".") == 0 || std::strcmp(child->d_name, "..") == 0)
          continue;
        if (++count > kAdminMaxDirectoryEntries)
          return sendResponseFrame(outFd, request.requestId, AdminStatusCode::InternalError, "Directory entry limit exceeded.");
        if ((count % 512ULL) == 0ULL) {
          if (checkStreamingCancelled(request.requestId, shouldExit)) {
            return !shouldExit && sendResponseFrame(
                outFd, request.requestId, AdminStatusCode::Cancelled, "Directory item count cancelled."
            );
          }
        }
      }

      AdminServerFrame frame;
      frame.kind = AdminServerFrameKind::CountResult;
      frame.requestId = request.requestId;
      frame.itemCount = count;
      return sendServerFrame(outFd, frame);
    };

    auto validateAuthorizedDirectoryForWrite = [&](
        const std::string& requestPath, AdminStatusCode& statusOut, std::string& detailOut,
        std::uint64_t extraResolve = 0
    ) -> bool {
      if (authorizedDir.fd.fd < 0 || requestPath.empty() || requestPath != authorizedDir.path) {
        statusOut = AdminStatusCode::Unauthorized;
        detailOut = "The target directory is not authorised in this Administrator session.";
        return false;
      }

      struct stat pinnedSt{};
      if (::fstat(authorizedDir.fd.fd, &pinnedSt) != 0) {
        const int err = errno;
        statusOut = statusCodeFromErrno(err);
        detailOut = std::strerror(err);
        return false;
      }
      if (!S_ISDIR(pinnedSt.st_mode)) {
        statusOut = AdminStatusCode::NotDirectory;
        detailOut = "The authorised path is no longer a directory.";
        return false;
      }
      if (pinnedSt.st_nlink == 0) {
        statusOut = AdminStatusCode::NotFound;
        detailOut = "The authorised directory no longer exists.";
        return false;
      }
      if (static_cast<std::uint64_t>(pinnedSt.st_dev) != authorizedDir.device
          || static_cast<std::uint64_t>(pinnedSt.st_ino) != authorizedDir.inode
          || pinnedSt.st_uid != authorizedDir.ownerUid
          || pinnedSt.st_gid != authorizedDir.ownerGid) {
        statusOut = AdminStatusCode::IdentityMismatch;
        detailOut = "The authorised directory changed or was replaced.";
        return false;
      }

      const ScopedFd freshFd(
          openAt2NoMagiclinks(AT_FDCWD, authorizedDir.path.c_str(), O_PATH | O_DIRECTORY, extraResolve)
      );
      if (freshFd.fd < 0) {
        const int err = errno;
        statusOut = statusCodeFromErrno(err);
        detailOut = std::strerror(err);
        return false;
      }
      struct stat freshSt{};
      if (::fstat(freshFd.fd, &freshSt) != 0) {
        const int err = errno;
        statusOut = statusCodeFromErrno(err);
        detailOut = std::strerror(err);
        return false;
      }
      if (!S_ISDIR(freshSt.st_mode)
          || static_cast<std::uint64_t>(freshSt.st_dev) != authorizedDir.device
          || static_cast<std::uint64_t>(freshSt.st_ino) != authorizedDir.inode
          || freshSt.st_uid != authorizedDir.ownerUid
          || freshSt.st_gid != authorizedDir.ownerGid) {
        statusOut = AdminStatusCode::IdentityMismatch;
        detailOut = "The authorised directory path no longer resolves to the same directory.";
        return false;
      }

      struct statvfs vfs{};
      if (::fstatvfs(authorizedDir.fd.fd, &vfs) == 0 && (vfs.f_flag & ST_RDONLY) != 0) {
        statusOut = AdminStatusCode::ReadOnlyFilesystem;
        detailOut = "The filesystem is mounted read-only.";
        return false;
      }
      if (::faccessat(authorizedDir.fd.fd, "", W_OK | X_OK, AT_EMPTY_PATH | AT_EACCESS) != 0) {
        const int err = errno;
        statusOut = err == EROFS ? AdminStatusCode::ReadOnlyFilesystem : AdminStatusCode::AccessDenied;
        detailOut = err == EROFS ? "The filesystem is mounted read-only." : std::strerror(err);
        return false;
      }
      return true;
    };

    auto executeCopy = [&](const AdminClientRequest& request, bool& shouldExit) -> bool {
      const bool singleFile = request.command == AdminClientCommand::CopyFile;
      const AdminCopySource singleSource{request.sourcePath, request.expectedDevice, request.expectedInode, false};
      const auto sources = singleFile ? std::span(&singleSource, 1) : std::span(request.copySources);
      CopyTreeStage tree;
      AdminStatusCode status = AdminStatusCode::Ok;
      std::string detail;
      const auto error = [&](AdminStatusCode code, std::string_view message) {
        status = code;
        detail = message;
        return false;
      };
      const auto ioError = [&](int err) { return error(statusCodeFromErrno(err), std::strerror(err)); };
      const auto changed = [&] { return error(AdminStatusCode::IdentityMismatch, "A source changed during copy."); };
      const auto stopped = [&](bool checkDeferred = false) {
        if (!checkStreamingCancelled(request.requestId, shouldExit, checkDeferred))
          return false;
        if (shouldExit && result.reason != AdminHelperSessionExitReason::CleanShutdown
            && result.reason != AdminHelperSessionExitReason::ClientEof)
          error(AdminStatusCode::InternalError, result.detail);
        else
          error(AdminStatusCode::Cancelled, "Copy cancelled.");
        return true;
      };
      const auto authorized = [&] {
        if (request.directoryListingId != authorizedDir.listingId)
          return error(AdminStatusCode::Unauthorized, "The authorised directory listing changed.");
        return validateAuthorizedDirectoryForWrite(request.path, status, detail, RESOLVE_NO_SYMLINKS);
      };

      struct CopyNode {
        std::string path; // Relative to a pinned source root and to the staged tree root.
        struct stat info{};
      };
      struct CopyRoot {
        AdminCopySource source;
        std::string name;
        std::vector<CopyNode> nodes;
      };
      std::vector<CopyRoot> roots;
      std::uint64_t totalBytes = 0;
      std::uint64_t totalItems = 0;
      std::uint64_t copied = 0;
      std::uint64_t completedItems = 0;
      std::size_t manifestPathBytes = 0;
      auto lastReport = std::chrono::steady_clock::now();
      const auto progress = [&](std::string_view item) {
        lastReport = std::chrono::steady_clock::now();
        return sendServerFrame(
            outFd,
            {
                .kind = AdminServerFrameKind::Progress,
                .requestId = request.requestId,
                .completedItems = singleFile ? 0 : completedItems,
                .totalItems = totalItems,
                .completedBytes = copied,
                .totalBytes = totalBytes,
                .currentItem = std::string(item),
            }
        );
      };
      const auto periodicProgress = [&](std::string_view item) {
        return std::chrono::steady_clock::now() - lastReport < std::chrono::milliseconds(100) || progress(item);
      };

      const auto runCopy = [&]() -> bool {
        if (!authorized() || stopped(true))
          return false;
        const int destFd = authorizedDir.fd.fd;
        const ScopedFd procFd(::open("/proc/self/fd", O_PATH | O_DIRECTORY | O_CLOEXEC));
        struct statfs procFs{};
        if (procFd.fd < 0 || ::fstatfs(procFd.fd, &procFs) != 0 || procFs.f_type != PROC_SUPER_MAGIC)
          return error(AdminStatusCode::NotSupported, "Copy requires access to the helper's procfs descriptors.");

        std::optional<gid_t> primaryGroup;
        if (::geteuid() == 0) {
          const auto* account = ::getpwuid(config.invokingUid);
          if (!account)
            return error(AdminStatusCode::AccessDenied, "Could not resolve the invoking user's primary group.");
          primaryGroup = account->pw_gid;
        }

        const auto openSource = [&](int parent, const char* path, bool root, bool readable, struct stat& info) {
          bool restored = true;
          auto fd = openAsInvoker(config.invokingUid, primaryGroup, [&]() -> ScopedFd {
            ScopedFd pinned(openAt2NoMagiclinks(parent, path, O_PATH, root ? RESOLVE_NO_SYMLINKS : kCopyChildResolve));
            if (pinned.fd < 0) {
              const int err = errno;
              if (err == ELOOP || err == EXDEV)
                error(AdminStatusCode::BadRequest, "Source symlinks and nested mounts cannot be copied.");
              else
                ioError(err);
              return ScopedFd{};
            }
            if (::fstat(pinned.fd, &info) != 0) {
              ioError(errno);
              return ScopedFd{};
            }
            if ((!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) || info.st_size < 0) {
              error(AdminStatusCode::BadRequest, "Only regular files and directories can be copied.");
              return ScopedFd{};
            }
            if (root && !supportedCopyFilesystem(pinned.fd)) {
              error(AdminStatusCode::NotSupported, "The source filesystem does not support Administrator Copy.");
              return ScopedFd{};
            }
            if (!readable)
              return pinned;
            // Reopen only our checked descriptor, never the caller's name.
            const int flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK | (S_ISDIR(info.st_mode) ? O_DIRECTORY : 0);
            ScopedFd input(::openat(procFd.fd, std::to_string(pinned.fd).c_str(), flags));
            if (input.fd < 0) {
              ioError(errno);
              return ScopedFd{};
            }
            struct stat opened{};
            if (::fstat(input.fd, &opened) != 0 || !sameCopyIdentity(info, opened)) {
              changed();
              return ScopedFd{};
            }
            return input;
          }, status, restored);
          if (!restored) {
            result.reason = AdminHelperSessionExitReason::IoError;
            result.detail = "Could not restore the helper's filesystem credentials.";
            shouldExit = true;
            error(AdminStatusCode::InternalError, result.detail);
            return ScopedFd{};
          }
          if (fd.fd >= 0) {
            status = AdminStatusCode::Ok;
            detail.clear();
          } else if (detail.empty()) {
            detail = "Could not access the source as the invoking user.";
          }
          return fd;
        };

        const auto rootMatches = [](const AdminCopySource& source, const struct stat& info) {
          return static_cast<std::uint64_t>(info.st_dev) == source.device
              && static_cast<std::uint64_t>(info.st_ino) == source.inode;
        };
        const auto rejectRecursiveDestination = [&] {
          return error(AdminStatusCode::BadRequest, "Cannot copy a directory into itself or a descendant.");
        };
        std::optional<CopyDirectoryKeys> ancestors;
        if (std::ranges::any_of(sources, &AdminCopySource::directory)) {
          ancestors = copyDestinationAncestors(destFd);
          if (!ancestors)
            return error(AdminStatusCode::IdentityMismatch, "Could not verify the destination ancestry.");
        }

        std::set<std::string> names;
        for (const auto& source : sources) {
          if (stopped())
            return false;
          auto name = std::filesystem::path(source.path).filename().string();
          if (!validWriteEntryName(name))
            return error(AdminStatusCode::BadRequest, "Invalid source filename.");
          if (!names.insert(name).second)
            return error(AdminStatusCode::AlreadyExists, "Multiple sources have the same destination name.");
          struct stat existing{};
          if (::fstatat(destFd, name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0)
            return error(AdminStatusCode::AlreadyExists, "An item with the name \"" + name + "\" already exists.");
          if (errno != ENOENT)
            return ioError(errno);

          struct stat info{};
          const auto input = openSource(AT_FDCWD, source.path.c_str(), true, true, info);
          if (input.fd < 0)
            return false;
          if (!rootMatches(source, info))
            return changed();
          if (source.directory != S_ISDIR(info.st_mode))
            return error(AdminStatusCode::BadRequest, "The source type does not match the requested type.");

          auto& root = roots.emplace_back(CopyRoot{source, std::move(name), {}});
          const auto scan = [&](auto&& self, std::string path, const struct stat& st, int fd, std::size_t depth) -> bool {
            manifestPathBytes += path.size();
            if (++totalItems > kAdminMaxDirectoryEntries || depth > kCopyMaxDepth
                || path.size() + 5 >= kAdminMaxPathBytes || manifestPathBytes > kCopyMaxManifestPathBytes)
              return error(AdminStatusCode::NotSupported, "The selection exceeds Administrator Copy traversal limits.");
            root.nodes.push_back({path, st});
            if (S_ISREG(st.st_mode)) {
              const auto size = static_cast<std::uint64_t>(st.st_size);
              if (size > UINT64_MAX - totalBytes)
                return error(AdminStatusCode::NotSupported, "The selection exceeds the supported byte count.");
              totalBytes += size;
              return true;
            }

            ScopedFd listing(::fcntl(fd, F_DUPFD_CLOEXEC, 0));
            ScopedDir dir(listing.fd >= 0 ? ::fdopendir(listing.fd) : nullptr);
            if (!dir.dir)
              return ioError(errno);
            (void)listing.release();
            if (ancestors->contains({st.st_dev, st.st_ino}))
              return rejectRecursiveDestination();
            while (true) {
              if (stopped())
                return false;
              errno = 0;
              const auto* entry = ::readdir(dir.dir);
              if (!entry) {
                if (errno != 0)
                  return ioError(errno);
                break;
              }
              const std::string_view child(entry->d_name);
              if (child == "." || child == "..")
                continue;
              auto relative = path.empty() ? std::string(child) : path + "/" + std::string(child);
              struct stat childInfo{};
              const auto childFd = openSource(input.fd, relative.c_str(), false, true, childInfo);
              if (childFd.fd < 0 || !self(self, std::move(relative), childInfo, childFd.fd, depth + 1))
                return false;
            }
            struct stat after{};
            return (::fstat(fd, &after) == 0 && sameCopyRevision(st, after)) || changed();
          };
          if (!scan(scan, "", info, input.fd, 0))
            return false;
        }

        if (ancestors && (!supportedCopyFilesystem(destFd) || !canStageCopyTree(destFd)))
          return error(AdminStatusCode::NotSupported, "Directory Copy requires a protected local destination directory.");
        tree.parent = destFd;
        if (!progress(roots.front().name))
          return false;

        std::array<char, 128 * 1024> buffer;
        for (const auto& root : roots) {
          if (stopped() || !authorized())
            return false;
          struct stat rootInfo{};
          const auto input = openSource(AT_FDCWD, root.source.path.c_str(), true, true, rootInfo);
          if (input.fd < 0 || !sameCopyRevision(rootInfo, root.nodes.front().info))
            return changed();

          ScopedFd stagedFile;
          const bool directory = root.source.directory;
          if (directory) {
            if (!canStageCopyTree(destFd))
              return error(AdminStatusCode::NotSupported, "The destination no longer supports private directory staging.");
            std::uint64_t nonce{};
            if (::getrandom(&nonce, sizeof(nonce), GRND_NONBLOCK) != sizeof(nonce))
              return error(AdminStatusCode::InternalError, "Could not allocate private directory staging.");
            auto stageName = std::format(".nythera-copy-{:016x}", nonce);
            if (::mkdirat(tree.parent, stageName.c_str(), 0700) != 0)
              return ioError(errno);
            tree.name = std::move(stageName);
            tree.directory = ScopedFd(openAt2NoMagiclinks(tree.parent, tree.name.c_str(), O_RDONLY | O_DIRECTORY, kCopyChildResolve));
            if (tree.directory.fd < 0)
              return ioError(errno);
            struct stat wrapper{};
            if (::fstat(tree.directory.fd, &wrapper) != 0)
              return ioError(errno);
            if (wrapper.st_uid != ::geteuid() || (wrapper.st_mode & 0777) != 0700)
              return error(AdminStatusCode::NotSupported, "The destination cannot create a private staging directory.");
            // Do not carry a parent's default ACL into published descendants.
            if (::fremovexattr(tree.directory.fd, "system.posix_acl_default") != 0
                && errno != ENODATA && errno != EOPNOTSUPP)
              return ioError(errno);
          } else {
            stagedFile = ScopedFd(openAt2NoMagiclinks(destFd, ".", O_TMPFILE | O_WRONLY, kCopyChildResolve, 0600));
            if (stagedFile.fd < 0) {
              if (errno == EOPNOTSUPP || errno == EISDIR || errno == EINVAL)
                return error(AdminStatusCode::NotSupported, "The destination filesystem does not support private copy staging.");
              return ioError(errno);
            }
          }

          for (const auto& node : root.nodes) {
            if (stopped())
              return false;
            const auto& before = node.info;
            const auto stagePath = node.path.empty() ? "item" : "item/" + node.path;
            if (S_ISDIR(before.st_mode)) {
              if (::mkdirat(tree.directory.fd, stagePath.c_str(), 0700) != 0)
                return ioError(errno);
            } else {
              struct stat current{};
              const auto child = node.path.empty() ? ScopedFd{} : openSource(input.fd, node.path.c_str(), false, true, current);
              const int sourceFd = node.path.empty() ? input.fd : child.fd;
              if (sourceFd < 0 || !sameCopyRevision(node.path.empty() ? rootInfo : current, before))
                return changed();
              const ScopedFd output(directory
                  ? openAt2NoMagiclinks(tree.directory.fd, stagePath.c_str(), O_WRONLY | O_CREAT | O_EXCL, kCopyChildResolve, 0600)
                  : -1);
              if (directory && output.fd < 0)
                return ioError(errno);
              const int targetFd = directory ? output.fd : stagedFile.fd;
              auto remaining = static_cast<std::uint64_t>(before.st_size);
              while (remaining != 0) {
                if (config.copyCheckpointForTesting)
                  config.copyCheckpointForTesting(copied);
                if (stopped())
                  return false;
                const ssize_t n = ::read(sourceFd, buffer.data(), std::min<std::uint64_t>(buffer.size(), remaining));
                if (n < 0 && errno == EINTR)
                  continue;
                if (n <= 0)
                  return n == 0 ? changed() : ioError(errno);
                ssize_t written = 0;
                while (written < n) {
                  if (stopped())
                    return false;
                  const ssize_t count = ::write(targetFd, buffer.data() + written, n - written);
                  if (count < 0 && errno == EINTR)
                    continue;
                  if (count <= 0)
                    return ioError(count < 0 ? errno : EIO);
                  written += count;
                }
                copied += static_cast<std::uint64_t>(n);
                remaining -= static_cast<std::uint64_t>(n);
                if (!periodicProgress(root.name))
                  return false;
              }
              char extra;
              ssize_t end;
              do {
                if (stopped())
                  return false;
                end = ::read(sourceFd, &extra, 1);
              } while (end < 0 && errno == EINTR);
              if (end != 0)
                return end < 0 ? ioError(errno) : changed();
              const timespec times[]{before.st_atim, before.st_mtim};
              if (::fchmod(targetFd, before.st_mode & 0755) != 0 || ::futimens(targetFd, times) != 0 || ::fsync(targetFd) != 0)
                return ioError(errno);
            }
            ++completedItems;
            if (!periodicProgress(root.name))
              return false;
          }

          struct stat staged{};
          if (directory) {
            // Finalise directories bottom-up. The helper keeps owner access,
            // including the ability to clean the private tree on failure.
            for (const auto& node : root.nodes | std::views::reverse) {
              if (stopped())
                return false;
              if (!S_ISDIR(node.info.st_mode))
                continue;
              const auto stagePath = node.path.empty() ? "item" : "item/" + node.path;
              ScopedFd dir(openAt2NoMagiclinks(tree.directory.fd, stagePath.c_str(), O_RDONLY | O_DIRECTORY, kCopyChildResolve));
              const timespec times[]{node.info.st_atim, node.info.st_mtim};
              if (dir.fd < 0 || ::fchmod(dir.fd, 0700 | (node.info.st_mode & 0055)) != 0
                  || ::futimens(dir.fd, times) != 0)
                return ioError(errno);
            }
            if (::fstatat(tree.directory.fd, "item", &staged, AT_SYMLINK_NOFOLLOW) != 0)
              return ioError(errno);
          } else if (::fstat(stagedFile.fd, &staged) != 0) {
            return ioError(errno);
          }

          if (!progress(root.name) || !authorized() || stopped())
            return false;
          if (directory) {
            const auto finalAncestors = copyDestinationAncestors(destFd);
            if (!finalAncestors)
              return error(AdminStatusCode::IdentityMismatch, "Could not verify the destination ancestry.");
            for (const auto& node : root.nodes) {
              if (S_ISDIR(node.info.st_mode) && finalAncestors->contains({node.info.st_dev, node.info.st_ino}))
                return rejectRecursiveDestination();
            }
          }

          // Reopen the original root under invoking credentials and check every
          // staged source path, including directory revisions and mount boundaries.
          struct stat freshInfo{};
          const auto fresh = openSource(AT_FDCWD, root.source.path.c_str(), true, false, freshInfo);
          if (fresh.fd < 0 || !sameCopyRevision(freshInfo, rootInfo))
            return changed();
          for (const auto& node : root.nodes | std::views::reverse) {
            if (stopped())
              return false;
            if (node.path.empty())
              continue;
            struct stat current{};
            const auto check = openSource(fresh.fd, node.path.c_str(), false, false, current);
            if (check.fd < 0 || !sameCopyRevision(current, node.info))
              return changed();
          }

          struct stat finalInfo{};
          const auto finalRoot = openSource(AT_FDCWD, root.source.path.c_str(), true, false, finalInfo);
          if (finalRoot.fd < 0 || !sameCopyRevision(finalInfo, rootInfo))
            return changed();
          if (!authorized() || stopped())
            return false;

          int publication;
          if (directory) {
            publication = static_cast<int>(::syscall(
                SYS_renameat2, tree.directory.fd, "item", destFd, root.name.c_str(), RENAME_NOREPLACE
            ));
          } else {
            publication = ::linkat(procFd.fd, std::to_string(stagedFile.fd).c_str(), destFd, root.name.c_str(), AT_SYMLINK_FOLLOW);
          }
          if (publication != 0) {
            if (errno == EEXIST)
              return error(AdminStatusCode::AlreadyExists, "An item with the name \"" + root.name + "\" already exists.");
            if (directory && (errno == EXDEV || errno == EINVAL || errno == EOPNOTSUPP || errno == ENOSYS))
              return error(AdminStatusCode::NotSupported, "This destination does not support non-replacing directory publication.");
            return ioError(errno);
          }

          if (!sendServerFrame(outFd, {
                  .kind = singleFile ? AdminServerFrameKind::MutationResult : AdminServerFrameKind::CopyItem,
                  .requestId = request.requestId,
                  .mutationInfo = {root.name, static_cast<std::uint64_t>(staged.st_dev), static_cast<std::uint64_t>(staged.st_ino)},
              }, true))
            return false;
          if (!tree.clean())
            return error(AdminStatusCode::InternalError, "Could not remove private staging directory " + tree.name + ".");
        }
        return true;
      };

      const bool success = runCopy();
      if (!tree.clean()) {
        status = AdminStatusCode::InternalError;
        detail = "Could not remove private staging directory " + tree.name + ".";
      }
      if (singleFile && success && status == AdminStatusCode::Ok)
        return true; // Legacy single-file request ends with MUTATION_RESULT.
      if (!success && status == AdminStatusCode::Ok)
        return false;
      return sendServerFrame(outFd, {
          .kind = AdminServerFrameKind::Response, .requestId = request.requestId, .status = status, .detail = detail,
      }, true);
    };

    auto executeCreateFile = [&](const AdminClientRequest& request) -> bool {
      if (!validWriteEntryName(request.entryName)) {
        return sendResponseFrame(outFd, request.requestId, AdminStatusCode::BadRequest, "Invalid file name.");
      }
      AdminStatusCode authStatus = AdminStatusCode::Ok;
      std::string authDetail;
      if (!validateAuthorizedDirectoryForWrite(request.path, authStatus, authDetail))
        return sendResponseFrame(outFd, request.requestId, authStatus, authDetail);

      std::string stem;
      std::string ext;
      splitStemAndExtension(request.entryName, stem, ext);
      const unsigned maxAttempts = request.autoNumber ? 1000U : 1U;
      ScopedCreationUmask umaskGuard(config.creationUmask);

      for (unsigned suffix = 1; suffix <= maxAttempts; ++suffix) {
        std::string candidate = suffix == 1 ? request.entryName : stem + " " + std::to_string(suffix) + ext;
        if (!validWriteEntryName(candidate)) {
          return sendResponseFrame(
              outFd, request.requestId, AdminStatusCode::BadRequest, "Generated filename exceeds allowed length."
          );
        }
        const ScopedFd createdFd(openAt2NoMagiclinks(
            authorizedDir.fd.fd, candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,
            RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS, 0666
        ));
        if (createdFd.fd >= 0) {
          struct stat createdSt{};
          if (::fstat(createdFd.fd, &createdSt) != 0 || !S_ISREG(createdSt.st_mode)) {
            return sendResponseFrame(
                outFd, request.requestId, AdminStatusCode::InternalError,
                "Created entry could not be verified as a regular file."
            );
          }
          AdminServerFrame frame;
          frame.kind = AdminServerFrameKind::MutationResult;
          frame.requestId = request.requestId;
          frame.mutationInfo.name = std::move(candidate);
          frame.mutationInfo.device = static_cast<std::uint64_t>(createdSt.st_dev);
          frame.mutationInfo.inode = static_cast<std::uint64_t>(createdSt.st_ino);
          return sendServerFrame(outFd, frame);
        }

        const int err = errno;
        bool existsCollision = (err == EEXIST);
        if (err == ELOOP) {
          struct stat linkSt{};
          if (::fstatat(authorizedDir.fd.fd, candidate.c_str(), &linkSt, AT_SYMLINK_NOFOLLOW) == 0
              && S_ISLNK(linkSt.st_mode)) {
            existsCollision = true;
          }
        }
        if (existsCollision) {
          if (request.autoNumber)
            continue;
          return sendResponseFrame(
              outFd, request.requestId, AdminStatusCode::AlreadyExists, "An item with that name already exists."
          );
        }
        return sendResponseFrame(outFd, request.requestId, statusCodeFromErrno(err), std::strerror(err));
      }

      return sendResponseFrame(
          outFd, request.requestId, AdminStatusCode::AlreadyExists, "An item with that name already exists."
      );
    };

    auto executeCreateDir = [&](const AdminClientRequest& request) -> bool {
      if (!validWriteEntryName(request.entryName)) {
        return sendResponseFrame(outFd, request.requestId, AdminStatusCode::BadRequest, "Invalid folder name.");
      }
      AdminStatusCode authStatus = AdminStatusCode::Ok;
      std::string authDetail;
      if (!validateAuthorizedDirectoryForWrite(request.path, authStatus, authDetail))
        return sendResponseFrame(outFd, request.requestId, authStatus, authDetail);

      std::string stem;
      std::string ext;
      splitStemAndExtension(request.entryName, stem, ext);
      const unsigned maxAttempts = request.autoNumber ? 1000U : 1U;
      ScopedCreationUmask umaskGuard(config.creationUmask);

      for (unsigned suffix = 1; suffix <= maxAttempts; ++suffix) {
        std::string candidate = suffix == 1 ? request.entryName : stem + " " + std::to_string(suffix) + ext;
        if (!validWriteEntryName(candidate)) {
          return sendResponseFrame(
              outFd, request.requestId, AdminStatusCode::BadRequest, "Generated folder name exceeds allowed length."
          );
        }
        if (::mkdirat(authorizedDir.fd.fd, candidate.c_str(), 0777) == 0) {
          EntryMetadata createdMeta;
          if (!queryEntryMetadataAt(authorizedDir.fd.fd, candidate.c_str(), 0, createdMeta)
              || !S_ISDIR(createdMeta.mode)) {
            return sendResponseFrame(
                outFd, request.requestId, AdminStatusCode::InternalError, "Created folder could not be verified."
            );
          }
          AdminServerFrame frame;
          frame.kind = AdminServerFrameKind::MutationResult;
          frame.requestId = request.requestId;
          frame.mutationInfo.name = std::move(candidate);
          frame.mutationInfo.device = createdMeta.device;
          frame.mutationInfo.inode = createdMeta.inode;
          return sendServerFrame(outFd, frame);
        }

        const int err = errno;
        if (err == EEXIST) {
          if (request.autoNumber)
            continue;
          return sendResponseFrame(
              outFd, request.requestId, AdminStatusCode::AlreadyExists, "An item with that name already exists."
          );
        }
        return sendResponseFrame(outFd, request.requestId, statusCodeFromErrno(err), std::strerror(err));
      }

      return sendResponseFrame(
          outFd, request.requestId, AdminStatusCode::AlreadyExists, "An item with that name already exists."
      );
    };

    auto executeRenameEntry = [&](const AdminClientRequest& request) -> bool {
      if (!validWriteEntryName(request.sourceName)
          || !validWriteEntryName(request.destName)
          || request.sourceName == request.destName
          || request.expectedDevice == 0
          || request.expectedInode == 0) {
        return sendResponseFrame(outFd, request.requestId, AdminStatusCode::BadRequest, "Invalid rename request.");
      }
      AdminStatusCode authStatus = AdminStatusCode::Ok;
      std::string authDetail;
      if (!validateAuthorizedDirectoryForWrite(request.path, authStatus, authDetail))
        return sendResponseFrame(outFd, request.requestId, authStatus, authDetail);

      struct stat srcSt{};
      if (::fstatat(authorizedDir.fd.fd, request.sourceName.c_str(), &srcSt, AT_SYMLINK_NOFOLLOW) != 0) {
        const int err = errno;
        return sendResponseFrame(outFd, request.requestId, statusCodeFromErrno(err), std::strerror(err));
      }
      if (static_cast<std::uint64_t>(srcSt.st_dev) != authorizedDir.device) {
        return sendResponseFrame(
            outFd, request.requestId, AdminStatusCode::AccessDenied, "Cannot rename a mount point."
        );
      }
      const bool srcTypeMatches = request.expectedIsSymlink
          ? S_ISLNK(srcSt.st_mode)
          : (!S_ISLNK(srcSt.st_mode) && request.expectedIsDir == S_ISDIR(srcSt.st_mode));
      if (static_cast<std::uint64_t>(srcSt.st_dev) != request.expectedDevice
          || static_cast<std::uint64_t>(srcSt.st_ino) != request.expectedInode
          || !srcTypeMatches) {
        return sendResponseFrame(
            outFd, request.requestId, AdminStatusCode::IdentityMismatch,
            "The source item changed before it could be renamed."
        );
      }

      struct stat dstPreSt{};
      if (::fstatat(authorizedDir.fd.fd, request.destName.c_str(), &dstPreSt, AT_SYMLINK_NOFOLLOW) == 0) {
        return sendResponseFrame(
            outFd, request.requestId, AdminStatusCode::AlreadyExists, "An item with that name already exists."
        );
      }
      if (errno != ENOENT) {
        const int err = errno;
        return sendResponseFrame(outFd, request.requestId, statusCodeFromErrno(err), std::strerror(err));
      }

      if (config.beforeRenameHookForTesting)
        config.beforeRenameHookForTesting(request.path, request.sourceName, request.destName);

      const auto doRenameAt2 = [&](int oldDirFd, const char* oldName, int newDirFd, const char* newName,
                                   unsigned int flags) -> int {
        if (config.renameAt2HookForTesting)
          return config.renameAt2HookForTesting(oldDirFd, oldName, newDirFd, newName, flags);
        return static_cast<int>(syscall(SYS_renameat2, oldDirFd, oldName, newDirFd, newName, flags));
      };

      if (doRenameAt2(
              authorizedDir.fd.fd, request.sourceName.c_str(), authorizedDir.fd.fd, request.destName.c_str(),
              RENAME_NOREPLACE
          )
          != 0) {
        const int err = errno;
        if (err == ENOSYS
            || err == EOPNOTSUPP
#if defined(ENOTSUP) && ENOTSUP != EOPNOTSUPP
            || err == ENOTSUP
#endif
            || err == EINVAL) {
          return sendResponseFrame(
              outFd, request.requestId, AdminStatusCode::NotSupported,
              "The filesystem does not support atomic non-overwriting rename (RENAME_NOREPLACE)."
          );
        }
        if (err == EEXIST) {
          return sendResponseFrame(
              outFd, request.requestId, AdminStatusCode::AlreadyExists, "An item with that name already exists."
          );
        }
        return sendResponseFrame(outFd, request.requestId, statusCodeFromErrno(err), std::strerror(err));
      }

      struct stat dstPostSt{};
      if (::fstatat(authorizedDir.fd.fd, request.destName.c_str(), &dstPostSt, AT_SYMLINK_NOFOLLOW) != 0) {
        const int err = errno;
        return sendResponseFrame(outFd, request.requestId, statusCodeFromErrno(err), std::strerror(err));
      }
      const bool postTypeMatches = request.expectedIsSymlink
          ? S_ISLNK(dstPostSt.st_mode)
          : (!S_ISLNK(dstPostSt.st_mode) && request.expectedIsDir == S_ISDIR(dstPostSt.st_mode));
      if (static_cast<std::uint64_t>(dstPostSt.st_dev) != request.expectedDevice
          || static_cast<std::uint64_t>(dstPostSt.st_ino) != request.expectedInode
          || !postTypeMatches) {
        (void)doRenameAt2(
            authorizedDir.fd.fd, request.destName.c_str(), authorizedDir.fd.fd, request.sourceName.c_str(),
            RENAME_NOREPLACE
        );
        return sendResponseFrame(
            outFd, request.requestId, AdminStatusCode::IdentityMismatch,
            "The source item changed concurrently during rename."
        );
      }

      AdminServerFrame frame;
      frame.kind = AdminServerFrameKind::MutationResult;
      frame.requestId = request.requestId;
      frame.mutationInfo.name = request.destName;
      frame.mutationInfo.device = static_cast<std::uint64_t>(dstPostSt.st_dev);
      frame.mutationInfo.inode = static_cast<std::uint64_t>(dstPostSt.st_ino);
      return sendServerFrame(outFd, frame);
    };

    auto processLine = [&](std::string_view rawLine, bool& shouldExit) -> bool {
      shouldExit = false;
      AdminClientRequest request;
      AdminParseError parseError;
      if (!decodeAdminClientRequest(rawLine, request, &parseError)) {
        ++consecutiveBadRequests;
        const AdminStatusCode code = parseError.status == AdminParseStatus::UnsupportedCommand
            ? AdminStatusCode::Unsupported
            : AdminStatusCode::BadRequest;
        (void)sendResponseFrame(outFd, parseError.requestId, code, parseError.message);
        if (!result.handshakeCompleted || consecutiveBadRequests >= config.maxConsecutiveBadRequests) {
          result.reason = AdminHelperSessionExitReason::ProtocolViolation;
          result.detail = parseError.message;
          shouldExit = true;
        }
        return false;
      }

      consecutiveBadRequests = 0;

      if (!result.handshakeCompleted) {
        if (request.command != AdminClientCommand::Hello) {
          (void)sendResponseFrame(
              outFd, request.requestId, AdminStatusCode::HandshakeRequired, "HELLO handshake required before commands."
          );
          result.reason = AdminHelperSessionExitReason::ProtocolViolation;
          result.detail = "Command received before HELLO handshake.";
          shouldExit = true;
          return false;
        }
        if (request.protocolVersion != kAdminProtocolVersion) {
          (void)sendResponseFrame(
              outFd, request.requestId, AdminStatusCode::VersionMismatch, "Incompatible Administrator helper protocol."
          );
          result.reason = AdminHelperSessionExitReason::VersionMismatch;
          result.detail = "Protocol version mismatch.";
          shouldExit = true;
          return false;
        }

        activeInactivityTimeoutMs =
            clampNegotiatedInactivityTimeoutMs(config.inactivityTimeoutMs, request.inactivityTimeoutMs);
        result.negotiatedInactivityTimeoutMs = activeInactivityTimeoutMs;
        result.handshakeCompleted = true;
        lastActivity = sessionNow(config);
        ++result.requestsHandled;

        AdminServerFrame readyFrame;
        readyFrame.kind = AdminServerFrameKind::Ready;
        readyFrame.requestId = request.requestId;
        readyFrame.protocolVersion = kAdminProtocolVersion;
        readyFrame.helperPid = helperPid;
        readyFrame.invokingUid = config.invokingUid;
        readyFrame.nonce = request.nonce;
        if (!sendServerFrame(outFd, readyFrame)) {
          result.reason = AdminHelperSessionExitReason::IoError;
          result.detail = "Failed to write READY frame.";
          shouldExit = true;
          return false;
        }
        return true;
      }

      auto respond = [&](AdminStatusCode status, std::string_view detail = {}) {
        if (sendResponseFrame(outFd, request.requestId, status, detail))
          return true;
        result.reason = AdminHelperSessionExitReason::IoError;
        result.detail = "Failed to write shared operation response.";
        shouldExit = true;
        return false;
      };

      using Command = AdminClientCommand;
      const bool legacyMutation = request.command == Command::CreateFile
          || request.command == Command::CreateDir
          || request.command == Command::RenameEntry
          || request.command == Command::CopyFile
          || request.command == Command::CopyItems;
      if ((upload || sharedOperation) && legacyMutation)
        return respond(AdminStatusCode::BadRequest, "Another Administrator operation is active.");

      // Only meaningful user-initiated requests renew the helper's inactivity clock.
      // Automatic polling, PING keepalives, background statvfs/folder-counts, and cancellation do not.
      const bool renewsActivity = request.command == AdminClientCommand::RenewActivity
          || (request.command == AdminClientCommand::ListDir && request.userInitiated)
          || (request.command == AdminClientCommand::StatInfo && request.userInitiated)
          || request.command == AdminClientCommand::CreateFile
          || request.command == AdminClientCommand::CreateDir
          || request.command == AdminClientCommand::RenameEntry
          || request.command == AdminClientCommand::CopyFile
          || request.command == AdminClientCommand::CopyItems;
      if (renewsActivity)
        lastActivity = sessionNow(config);

      ++result.requestsHandled;
      switch (request.command) {
      case AdminClientCommand::Hello:
        if (!sendResponseFrame(
                outFd, request.requestId, AdminStatusCode::BadRequest, "Session handshake already completed."
            )) {
          result.reason = AdminHelperSessionExitReason::IoError;
          result.detail = "Failed to write response frame.";
          shouldExit = true;
          return false;
        }
        return true;

      case AdminClientCommand::Ping: {
        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(sessionNow(config) - sessionStart).count();
        AdminServerFrame statusFrame;
        statusFrame.kind = AdminServerFrameKind::Status;
        statusFrame.requestId = request.requestId;
        statusFrame.protocolVersion = kAdminProtocolVersion;
        statusFrame.helperPid = helperPid;
        statusFrame.invokingUid = config.invokingUid;
        statusFrame.uptimeMs = elapsedMs > 0 ? static_cast<std::uint64_t>(elapsedMs) : 0ULL;
        statusFrame.activeRequestId = sharedOperation ? sharedOperation->id() : kInvalidAdminRequestId;
        if (!sendServerFrame(outFd, statusFrame)) {
          result.reason = AdminHelperSessionExitReason::IoError;
          result.detail = "Failed to write STATUS frame.";
          shouldExit = true;
          return false;
        }
        return true;
      }

      case AdminClientCommand::RenewActivity:
        if (!sendResponseFrame(outFd, request.requestId, AdminStatusCode::Ok, {})) {
          result.reason = AdminHelperSessionExitReason::IoError;
          result.detail = "Failed to write RENEW_ACTIVITY response.";
          shouldExit = true;
          return false;
        }
        return true;

      case AdminClientCommand::Cancel:
        if (upload && upload->id == request.targetRequestId) {
          upload.reset();
          if (!sendServerFrame(
                  outFd,
                  {.kind = AdminServerFrameKind::OpResult,
                   .requestId = request.targetRequestId,
                   .status = AdminStatusCode::Cancelled}
              )) {
            result.reason = AdminHelperSessionExitReason::IoError;
            result.detail = "Failed to write cancelled upload result.";
            shouldExit = true;
            return false;
          }
        } else {
          cancelShared(request.targetRequestId);
        }
        result.lastCancelledTargetId = request.targetRequestId;
        if (!sendResponseFrame(outFd, request.requestId, AdminStatusCode::Ok, "Cancellation acknowledged.")) {
          result.reason = AdminHelperSessionExitReason::IoError;
          result.detail = "Failed to write CANCEL response.";
          shouldExit = true;
          return false;
        }
        return true;

      case AdminClientCommand::Shutdown:
        if (sharedOperation) {
          sharedOperation->cancelAndJoin();
          (void)flushShared();
        }
        (void)sendResponseFrame(
            outFd, request.requestId, AdminStatusCode::ShuttingDown, "Helper session terminated."
        );
        result.reason = AdminHelperSessionExitReason::CleanShutdown;
        result.detail = "Clean shutdown requested.";
        shouldExit = true;
        return true;

      case AdminClientCommand::ListDir:
        if (!executeListDir(request, shouldExit) && !shouldExit) {
          result.reason = AdminHelperSessionExitReason::IoError;
          result.detail = "Failed to write LIST_DIR response.";
          shouldExit = true;
          return false;
        }
        return !shouldExit;

      case AdminClientCommand::StatInfo:
        if (!executeStatInfo(request)) {
          result.reason = AdminHelperSessionExitReason::IoError;
          result.detail = "Failed to write STAT_INFO response.";
          shouldExit = true;
          return false;
        }
        return true;

      case AdminClientCommand::StatVfs:
        if (!executeStatVfs(request)) {
          result.reason = AdminHelperSessionExitReason::IoError;
          result.detail = "Failed to write STATVFS_INFO response.";
          shouldExit = true;
          return false;
        }
        return true;

      case AdminClientCommand::CountDir:
        if (!executeCountDir(request, shouldExit) && !shouldExit) {
          result.reason = AdminHelperSessionExitReason::IoError;
          result.detail = "Failed to write COUNT_DIR response.";
          shouldExit = true;
          return false;
        }
        return !shouldExit;

      case AdminClientCommand::CreateFile:
        if (!executeCreateFile(request)) {
          result.reason = AdminHelperSessionExitReason::IoError;
          result.detail = "Failed to write CREATE_FILE response.";
          shouldExit = true;
          return false;
        }
        return true;

      case AdminClientCommand::CreateDir:
        if (!executeCreateDir(request)) {
          result.reason = AdminHelperSessionExitReason::IoError;
          result.detail = "Failed to write CREATE_DIR response.";
          shouldExit = true;
          return false;
        }
        return true;

      case AdminClientCommand::RenameEntry:
        if (!executeRenameEntry(request)) {
          result.reason = AdminHelperSessionExitReason::IoError;
          result.detail = "Failed to write RENAME_ENTRY response.";
          shouldExit = true;
          return false;
        }
        return true;

      case AdminClientCommand::CopyFile:
      case AdminClientCommand::CopyItems:
        if (!executeCopy(request, shouldExit) && !shouldExit) {
          result.reason = AdminHelperSessionExitReason::IoError;
          result.detail = "Failed to write Administrator Copy response.";
          shouldExit = true;
        }
        return !shouldExit;

      case Command::OpBegin:
        if (upload || sharedOperation)
          return respond(AdminStatusCode::BadRequest, "Another Administrator operation is active.");
        upload.emplace(
            OperationUpload{
                .id = request.requestId,
                .count = request.sourceCount,
                .request = {
                    .kind = request.operationKind, .destination = std::move(request.path), .keepBoth = request.keepBoth
                }
            }
        );
        if (request.expectedInode != 0)
          upload->request.renameIdentity = {request.expectedDevice, request.expectedInode};
        upload->request.sources.reserve(upload->count);
        return true;
      case Command::OpSource:
        if (!upload || upload->id != request.requestId)
          return respond(AdminStatusCode::BadRequest, "No matching operation upload.");
        if (upload->request.sources.size() == upload->count
            || !validOperationSource(upload->request.kind, request.path)) {
          upload.reset();
          return respond(AdminStatusCode::BadRequest, "Invalid source for the shared operation.");
        }
        upload->request.sources.push_back(std::move(request.path));
        return true;
      case Command::OpExec: {
        if (!upload || upload->id != request.requestId)
          return respond(AdminStatusCode::BadRequest, "No matching operation upload.");
        auto operation = std::exchange(upload, std::nullopt);
        if (operation->request.sources.size() != operation->count
            || (operation->request.kind == engine::Kind::Rename
                && !validOperationSource(engine::Kind::Rename, operation->request.destination)))
          return respond(AdminStatusCode::BadRequest, "Incomplete or invalid shared operation.");
        try {
          auto worker = std::make_unique<SharedOperation>(request.requestId);
          if (worker->fd() < 0)
            return respond(AdminStatusCode::InternalError, "Could not create operation wakeup descriptor.");
          worker->start(std::move(operation->request), config);
          sharedOperation = std::move(worker);
          lastActivity = sessionNow(config);
        } catch (const std::system_error&) {
          return respond(AdminStatusCode::InternalError, "Could not start the shared operation.");
        }
        return true;
      }
      case Command::OpPause:
      case Command::OpResume:
      case Command::OpConflictReply:
        if (sharedControl(request))
          return true;
        result.reason = AdminHelperSessionExitReason::IoError;
        result.detail = "Failed to write shared operation control response.";
        shouldExit = true;
        return false;

      case AdminClientCommand::ConflictReply:
        if (!sendResponseFrame(
                outFd, request.requestId, AdminStatusCode::Unsupported,
                "Privileged conflict resolution is unavailable."
            )) {
          result.reason = AdminHelperSessionExitReason::IoError;
          result.detail = "Failed to write response frame.";
          shouldExit = true;
          return false;
        }
        return true;
      }
      return true;
    };

    bool sawEof = false;
    while (true) {
      if (remainingTimeMs() == 0) {
        drainExpiredOperation();
        return result;
      }

      if (!flushShared())
        return result;

      std::string frameLine;
      AdminFrameReadStatus status;
      if (!deferredLines.empty()) {
        frameLine = std::move(deferredLines.front());
        deferredLines.pop_front();
        status = AdminFrameReadStatus::FrameReady;
      } else {
        status = lineBuffer.popFrame(frameLine);
      }

      if (status == AdminFrameReadStatus::FrameReady) {
        bool shouldExit = false;
        (void)processLine(frameLine, shouldExit);
        if (shouldExit) {
          // Legacy writes can stop at expiry. Shared writes use their own bound,
          // so a failed shared write may have left a truncated frame on the pipe.
          if (result.reason == AdminHelperSessionExitReason::IoError && !sharedOperation)
            (void)remainingTimeMs();
          drainExpiredOperation();
          return result;
        }
        continue;
      }
      if (status == AdminFrameReadStatus::OversizedFrame) {
        (void)sendResponseFrame(outFd, 1, AdminStatusCode::BadRequest, "Protocol frame exceeds maximum allowed size.");
        result.reason = AdminHelperSessionExitReason::ProtocolViolation;
        result.detail = "Oversized protocol frame rejected.";
        return result;
      }
      if (status == AdminFrameReadStatus::MalformedFrame) {
        if (sawEof || !result.handshakeCompleted || ++consecutiveBadRequests >= config.maxConsecutiveBadRequests) {
          result.reason = AdminHelperSessionExitReason::ProtocolViolation;
          result.detail = "Malformed protocol frame rejected.";
          return result;
        }
        continue;
      }
      if (sawEof) {
        const bool truncated = lineBuffer.hasBufferedBytes();
        result.reason = truncated ? AdminHelperSessionExitReason::ProtocolViolation
                                  : AdminHelperSessionExitReason::ClientEof;
        result.detail = truncated ? "Truncated partial frame at EOF." : "Client closed stdin channel.";
        return result;
      }

      std::array<pollfd, 3> pfds{
          {{inFd, POLLIN, 0}, {sharedOperation ? sharedOperation->fd() : -1, POLLIN, 0}, {outFd, 0, 0}}
      };
      const int remaining = remainingTimeMs();
      const int pollRc =
          ::poll(pfds.data(), pfds.size(), sharedOperation && (remaining < 0 || remaining > 100) ? 100 : remaining);
      if (pollRc == 0 || (pollRc < 0 && errno == EINTR))
        continue;
      if (pollRc >= 0 && (pfds[2].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
        result.reason = AdminHelperSessionExitReason::ClientEof;
        result.detail = "Client closed the helper output channel.";
        return result;
      }
      if (pollRc < 0 || (pfds[0].revents & (POLLERR | POLLNVAL)) != 0) {
        result.reason = AdminHelperSessionExitReason::IoError;
        result.detail = "Could not poll the client channel.";
        return result;
      }

      if ((pfds[0].revents & (POLLIN | POLLHUP)) == 0)
        continue;

      std::array<char, 4096> chunk{};
      const ssize_t n = ::read(inFd, chunk.data(), chunk.size());
      if (n > 0) {
        lineBuffer.append(std::string_view(chunk.data(), static_cast<std::size_t>(n)));
      } else if (n == 0) {
        sawEof = true;
      } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
        result.reason = AdminHelperSessionExitReason::IoError;
        result.detail = std::strerror(errno);
        return result;
      }
    }
  }

} // namespace filemanager
