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
#include <iterator>
#include <limits.h>
#include <linux/openat2.h>
#include <memory>
#include <mutex>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
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
        int dirFd, const char* path, std::uint64_t flags, std::uint64_t extraResolve = 0
    ) noexcept {
      open_how how{};
      how.flags = flags | O_CLOEXEC;
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
            callbacks.transfer.report = [this, &config, renew = request.contentRevision != "thumbnail"](
                                            auto bytes, auto totalBytes, auto items, auto totalItems, auto speed
                                        ) {
              const auto reportedAt = sessionNow(config);
              std::lock_guard lock(m_mutex);
              if (renew && (bytes > m_progressBytes || items > m_progressItems)) {
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
              ScopedCreationUmask umaskGuard(engine::isProtectedRead(request.kind) ? std::nullopt : config.creationUmask);
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
                 .customDescription = std::move(execution.customDescription),
                 .properties = std::move(execution.properties)}
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
    std::array<std::unique_ptr<SharedOperation>, engine::kOperationSlots> operations;
    auto& sharedOperation = operations[0];
    const auto hasOperation = [&] { return std::ranges::any_of(operations, [](const auto& op) { return op != nullptr; }); };
    const auto findOperation = [&](AdminRequestId id) -> SharedOperation* {
      for (const auto& operation : operations)
        if (operation && operation->id() == id)
          return operation.get();
      return nullptr;
    };
    auto cancelShared = [&](AdminRequestId id) {
      if (auto* operation = findOperation(id))
        operation->cancel();
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
      for (const auto& operation : operations)
        if (operation)
          lastActivity = std::max(lastActivity, operation->lastProgress());
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
                               int fd, const AdminServerFrame& frame,
                               std::optional<SessionClock::time_point> writeDeadline = {}
                           ) {
      const auto encoded = encodeAdminServerFrame(frame);
      if (!encoded)
        return false;
      // Shared frames must remain writable during expiry, including a partially written frame.
      // Bound IPC backpressure separately from the session's idle deadline.
      if (!writeDeadline && hasOperation())
        writeDeadline = SessionClock::now() + sharedWriteTimeout;
      std::string_view remaining(*encoded);
      while (!remaining.empty()) {
        const int timeout = writeDeadline ? static_cast<int>(std::max(
                                 std::chrono::milliseconds::zero(),
                                 std::chrono::ceil<std::chrono::milliseconds>(*writeDeadline - SessionClock::now())
                             ).count())
                                          : remainingTimeMs();
        if (timeout == 0)
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
      for (auto& operation : operations) {
        if (!operation)
          continue;
        const auto writeDeadline = SessionClock::now() + sharedWriteTimeout;
        for (auto& frame : operation->takeFrames()) {
          if (expiring && frame.kind == AdminServerFrameKind::OpResult && frame.status == AdminStatusCode::Cancelled)
            frame.detail = frame.detail.empty() ? result.detail : result.detail + " " + frame.detail;
          if (!sendServerFrame(outFd, frame, writeDeadline)) {
            if (expiring)
              result.detail += " The final operation outcome could not be delivered.";
            else {
              result.reason = AdminHelperSessionExitReason::IoError;
              result.detail = "Failed to write shared operation frame.";
            }
            return false;
          }
          if (frame.kind == AdminServerFrameKind::OpResult) {
            lastActivity = std::max(lastActivity, operation->lastProgress());
            operation.reset();
          }
        }
      }

      return true;
    };

    auto drainOperations = [&](bool expiring = false) {
      for (const auto& operation : operations)
        if (operation)
          operation->cancelAndJoin();
      (void)flushShared(expiring);
    };
    auto drainExpiredOperation = [&] {
      if (result.reason == AdminHelperSessionExitReason::InactivityTimeout && hasOperation())
        drainOperations(true);
    };

    auto sharedControl = [&](const AdminClientRequest& request) {
      auto* active = findOperation(request.targetRequestId);
      if (!active)
        return sendResponseFrame(
            outFd, request.requestId, AdminStatusCode::BadRequest, "No matching shared operation."
        );
      if (request.command == AdminClientCommand::OpConflictReply) {
        if (!active->answer(request))
          return sendResponseFrame(
              outFd, request.requestId, AdminStatusCode::BadRequest, "No matching pending conflict."
          );
      } else {
        active->setPaused(request.command == AdminClientCommand::OpPause);
      }
      lastActivity = sessionNow(config);
      return sendResponseFrame(outFd, request.requestId, AdminStatusCode::Ok, {});
    };

    // Cancellation can overtake queued requests during enumeration. Keep the queue bounded
    // and process all other requests in their original order after the enumeration ends.
    constexpr std::size_t maxDeferredRequests = 64;
    std::deque<std::string> deferredLines;
    auto checkStreamingCancelled = [&](AdminRequestId activeReqId, bool& shouldExit) {
      if (remainingTimeMs() == 0 || !flushShared()) {
        shouldExit = true;
        return true;
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
            drainOperations();
            ++result.requestsHandled;
            (void)sendResponseFrame(
                outFd, incoming.requestId, AdminStatusCode::ShuttingDown, "Helper session terminated."
            );
            result.reason = AdminHelperSessionExitReason::CleanShutdown;
            shouldExit = true;
            return true;
          }
          if (incoming.command == AdminClientCommand::Cancel
              && findOperation(incoming.targetRequestId)) {
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

    auto executeListDir = [&](const AdminClientRequest& request, bool& shouldExit) -> bool {
      const int rawFd = openAt2NoMagiclinks(AT_FDCWD, request.path.c_str(), O_RDONLY | O_DIRECTORY | O_NOATIME);
      if (rawFd < 0) {
        const int err = errno;
        return sendResponseFrame(outFd, request.requestId, statusCodeFromErrno(err), std::strerror(err));
      }
      const bool writable = queryDirectoryWritableFd(rawFd);
      DIR* rawDir = ::fdopendir(rawFd);
      if (rawDir == nullptr) {
        const int err = errno;
        ::close(rawFd);
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

      return true;
    };

    auto executeStatInfo = [&](const AdminClientRequest& request) -> bool {
      const std::filesystem::path targetPath(request.path);
      AdminServerFrame frame;
      frame.kind = AdminServerFrameKind::StatResult;
      frame.requestId = request.requestId;

      if (request.statDetail == AdminStatDetail::DropSource || request.statDetail == AdminStatDetail::DropTarget) {
        const bool target = request.statDetail == AdminStatDetail::DropTarget;
        const auto directory = target ? targetPath : targetPath.parent_path();
        const ScopedFd directoryFd(openAt2NoMagiclinks(AT_FDCWD, directory.c_str(), O_PATH | O_DIRECTORY));
        if (directoryFd.fd < 0) {
          const int err = errno;
          return sendResponseFrame(outFd, request.requestId, statusCodeFromErrno(err), std::strerror(err));
        }
        const auto leaf = targetPath.filename();
        struct stat state{};
        const int statResult = target ? fstat(directoryFd.fd, &state)
            : fstatat(directoryFd.fd, leaf.c_str(), &state, AT_SYMLINK_NOFOLLOW);
        if (statResult != 0) {
          const int err = errno;
          return sendResponseFrame(outFd, request.requestId, statusCodeFromErrno(err), std::strerror(err));
        }
        const auto procPath = "/proc/self/fd/" + std::to_string(directoryFd.fd);
        std::array<char, PATH_MAX> resolved{};
        const auto length = readlink(procPath.c_str(), resolved.data(), resolved.size());
        if (length <= 0 || static_cast<std::size_t>(length) == resolved.size())
          return sendResponseFrame(outFd, request.requestId, AdminStatusCode::BadRequest, "Could not resolve drop path.");
        auto resolvedPath = std::filesystem::path(std::string_view(resolved.data(), static_cast<std::size_t>(length)));
        if (!target)
          resolvedPath /= leaf;
        if (resolvedPath.native().size() > kAdminMaxPathBytes)
          return sendResponseFrame(outFd, request.requestId, AdminStatusCode::BadRequest, "Drop path is too long.");
        frame.statInfo = {
            .exists = true,
            .isDir = S_ISDIR(state.st_mode),
            .isSymlink = S_ISLNK(state.st_mode),
            .writable = target && queryDirectoryWritableFd(directoryFd.fd),
            .device = static_cast<std::uint64_t>(state.st_dev),
            .resolvedPath = resolvedPath.string()
        };
        return sendServerFrame(outFd, frame);
      }

      if (request.statDetail == AdminStatDetail::Properties) {
        const bool root = targetPath == targetPath.root_path();
        const auto parent = root ? targetPath : targetPath.parent_path();
        const ScopedFd parentFd(openAt2NoMagiclinks(AT_FDCWD, parent.c_str(), O_PATH | O_DIRECTORY));
        if (parentFd.fd < 0) {
          const int err = errno;
          return sendResponseFrame(outFd, request.requestId, statusCodeFromErrno(err), std::strerror(err));
        }
        const auto pinnedPath = root ? request.path
            : "/proc/self/fd/" + std::to_string(parentFd.fd) + "/" + targetPath.filename().string();
        g_autoptr(GFile) file = g_file_new_for_path(pinnedPath.c_str());
        g_autoptr(GError) error = nullptr;
        g_autoptr(GFileInfo) info = g_file_query_info(
            file, kPropertyInfoAttributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, &error
        );
        if (!info)
          return sendResponseFrame(outFd, request.requestId, AdminStatusCode::InternalError, error->message);
        if (root)
          g_file_info_set_display_name(info, "/");
        frame.statInfo.properties = encodePropertyInfo(info);
        if (frame.statInfo.properties.size() > kAdminMaxFrameBytes / 2)
          return sendResponseFrame(outFd, request.requestId, AdminStatusCode::BadRequest, "Properties data is too large.");
        frame.statInfo.exists = true;
        return sendServerFrame(outFd, frame);
      }

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
      // Only meaningful user-initiated requests renew the helper's inactivity clock.
      // Automatic polling, PING keepalives, background statvfs/folder-counts, and cancellation do not.
      const bool renewsActivity = request.command == AdminClientCommand::RenewActivity
          || (request.command == AdminClientCommand::ListDir && request.userInitiated)
          || (request.command == AdminClientCommand::StatInfo && request.userInitiated);
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
        drainOperations();
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

      case Command::OpBegin:
        if (upload || operations[engine::operationSlot(request.operationKind, request.contentRevision)])
          return respond(AdminStatusCode::BadRequest, "Another Administrator operation is active.");
        upload.emplace(
            OperationUpload{
                .id = request.requestId,
                .count = request.sourceCount,
                .request = {
                    .kind = request.operationKind, .destination = std::move(request.path), .keepBoth = request.keepBoth,
                    .properties = std::move(request.properties), .contentRevision = std::move(request.contentRevision),
                    .invokingUid = config.invokingUid
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
          auto& slot = operations[engine::operationSlot(operation->request.kind, operation->request.contentRevision)];
          const bool renew = operation->request.contentRevision != "thumbnail";
          worker->start(std::move(operation->request), config);
          slot = std::move(worker);
          if (renew)
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
          if (result.reason == AdminHelperSessionExitReason::IoError && !hasOperation())
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

      std::array<pollfd, 2 + engine::kOperationSlots> pfds{{{inFd, POLLIN, 0}, {outFd, 0, 0}}};
      for (std::size_t i = 0; i < operations.size(); ++i)
        pfds[i + 2] = {operations[i] ? operations[i]->fd() : -1, POLLIN, 0};
      const int remaining = remainingTimeMs();
      const int pollRc =
          ::poll(pfds.data(), pfds.size(), hasOperation() && (remaining < 0 || remaining > 100) ? 100 : remaining);
      if (pollRc == 0 || (pollRc < 0 && errno == EINTR))
        continue;
      if (pollRc >= 0 && (pfds[1].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
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
