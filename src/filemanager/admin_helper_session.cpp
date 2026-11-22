#include "filemanager/admin_helper_session.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <limits.h>
#include <grp.h>
#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <linux/openat2.h>
#include <poll.h>
#include <pwd.h>
#include <sys/fsuid.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
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

    // The production helper is single-threaded. Resolve and open the source with the invoking
    // account's filesystem credentials, never with the helper's DAC privileges. Use only its
    // primary group, since pkexec does not authenticate the caller's supplementary group list.
    ScopedFd openCopySourceAsInvoker(
        uid_t uid, const AdminClientRequest& request, int procFd, AdminStatusCode& status, bool& credentialsRestored
    ) {
      auto openSource = [&]() -> ScopedFd {
        const ScopedFd pinned(openAt2NoMagiclinks(AT_FDCWD, request.sourcePath.c_str(), O_PATH, RESOLVE_NO_SYMLINKS));
        if (pinned.fd < 0) {
          status = statusCodeFromErrno(errno);
          return ScopedFd{};
        }
        struct stat st{};
        if (::fstat(pinned.fd, &st) != 0) {
          status = statusCodeFromErrno(errno);
          return ScopedFd{};
        }
        if (!S_ISREG(st.st_mode)) {
          status = AdminStatusCode::BadRequest;
          return ScopedFd{};
        }
        // Pseudo-files can advertise S_IFREG but perform credential-sensitive
        // work on read. Stage 4.1 accepts data files on known local filesystems.
        struct statfs sourceFs{};
        if (::fstatfs(pinned.fd, &sourceFs) != 0) {
          status = statusCodeFromErrno(errno);
          return ScopedFd{};
        }
        switch (sourceFs.f_type) {
        case EXT4_SUPER_MAGIC:
        case BTRFS_SUPER_MAGIC:
        case XFS_SUPER_MAGIC:
        case F2FS_SUPER_MAGIC:
        case OVERLAYFS_SUPER_MAGIC:
        case TMPFS_MAGIC:
        case RAMFS_MAGIC:
        case MSDOS_SUPER_MAGIC:
        case EXFAT_SUPER_MAGIC:
          break;
        default:
          status = AdminStatusCode::NotSupported;
          return ScopedFd{};
        }
        if (static_cast<std::uint64_t>(st.st_dev) != request.expectedDevice
            || static_cast<std::uint64_t>(st.st_ino) != request.expectedInode) {
          status = AdminStatusCode::IdentityMismatch;
          return ScopedFd{};
        }
        // This is a kernel-generated magic link to our own pinned fd. No
        // caller-controlled link is followed and a device/FIFO cannot be substituted.
        ScopedFd input(::openat(procFd, std::to_string(pinned.fd).c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
        if (input.fd < 0) {
          status = statusCodeFromErrno(errno);
          return ScopedFd{};
        }
        struct stat opened{};
        if (::fstat(input.fd, &opened) != 0
            || !S_ISREG(opened.st_mode)
            || opened.st_dev != st.st_dev
            || opened.st_ino != st.st_ino) {
          status = AdminStatusCode::IdentityMismatch;
          return ScopedFd{};
        }
        return input;
      };

      // In-process fixtures already run with the unprivileged invoking account.
      if (::geteuid() != 0 && ::geteuid() == uid)
        return openSource();

      status = AdminStatusCode::AccessDenied;
      if (::geteuid() != 0 || uid == 0)
        return ScopedFd{};
      const auto* account = ::getpwuid(uid);
      if (!account)
        return ScopedFd{};
      const gid_t primaryGroup = account->pw_gid;
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

      const auto oldGid = static_cast<gid_t>(::setfsgid(primaryGroup));
      const auto oldUid = static_cast<uid_t>(::setfsuid(uid));
      __user_cap_data_struct capData[2]{};
      constexpr unsigned dacCaps = (1U << CAP_DAC_OVERRIDE) | (1U << CAP_DAC_READ_SEARCH);
      ScopedFd input;
      if (static_cast<uid_t>(::setfsuid(static_cast<uid_t>(-1))) == uid
          && static_cast<gid_t>(::setfsgid(static_cast<gid_t>(-1))) == primaryGroup
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
        || setenv("LC_ALL", "C", 1) != 0) {
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
    const auto sessionStart = std::chrono::steady_clock::now();
    auto lastActivity = sessionStart;
    int activeInactivityTimeoutMs =
        clampNegotiatedInactivityTimeoutMs(config.inactivityTimeoutMs, 0);
    result.negotiatedInactivityTimeoutMs = activeInactivityTimeoutMs;

    AdminLineBuffer lineBuffer(config.maxFrameBytes);
    std::size_t consecutiveBadRequests = 0;

    auto remainingTimeMs = [&]() {
      const int timeout = result.handshakeCompleted ? activeInactivityTimeoutMs : config.handshakeTimeoutMs;
      if (timeout < 0)
        return -1;
      const auto from = result.handshakeCompleted ? lastActivity : sessionStart;
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - from
      ).count();
      if (elapsed < timeout)
        return static_cast<int>(timeout - elapsed);
      result.reason = result.handshakeCompleted ? AdminHelperSessionExitReason::InactivityTimeout
                                                : AdminHelperSessionExitReason::HandshakeTimeout;
      result.detail = result.handshakeCompleted ? "Session terminated due to inactivity timeout."
                                                : "Timed out waiting for HELLO handshake.";
      return 0;
    };

    auto sendServerFrame = [&](int fd, const AdminServerFrame& frame, bool committed = false) {
      const auto encoded = encodeAdminServerFrame(frame);
      if (!encoded)
        return false;
      std::string_view remaining(*encoded);
      while (!remaining.empty()) {
        const int timeout = remainingTimeMs();
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
        pollfd fds[]{{fd, POLLOUT, 0}, {inFd, 0, 0}};
        const int rc = ::poll(fds, 2, timeout);
        if (rc < 0 && errno == EINTR)
          continue;
        if (rc < 0
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

    // Cancellation can overtake queued requests during enumeration. Keep the queue bounded
    // and process all other requests in their original order after the enumeration ends.
    constexpr std::size_t maxDeferredRequests = 64;
    std::deque<std::string> deferredLines;
    auto checkStreamingCancelled = [&](AdminRequestId activeReqId, bool& shouldExit, bool checkDeferred = false) {
      if (remainingTimeMs() == 0) {
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
            ++result.requestsHandled;
            (void)sendResponseFrame(
                outFd, incoming.requestId, AdminStatusCode::ShuttingDown, "Helper session terminated."
            );
            result.reason = AdminHelperSessionExitReason::CleanShutdown;
            shouldExit = true;
            return true;
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

    auto executeCopyFile = [&](const AdminClientRequest& request, bool& shouldExit) -> bool {
      ScopedFd stage;
      ScopedFd input;
      const auto fail = [&](AdminStatusCode status, std::string_view detail) {
        stage = ScopedFd{};
        input = ScopedFd{};
        return sendResponseFrame(outFd, request.requestId, status, detail);
      };
      const auto failErrno = [&](int err) { return fail(statusCodeFromErrno(err), std::strerror(err)); };
      const auto cancelled = [&] { return fail(AdminStatusCode::Cancelled, "Copy cancelled."); };
      const std::string name = std::filesystem::path(request.sourcePath).filename().string();
      if (!validWriteEntryName(name))
        return fail(AdminStatusCode::BadRequest, "Invalid source filename.");
      if (request.directoryListingId != authorizedDir.listingId)
        return fail(AdminStatusCode::Unauthorized, "The authorised directory listing changed.");
      AdminStatusCode authStatus = AdminStatusCode::Ok;
      std::string authDetail;
      if (!validateAuthorizedDirectoryForWrite(request.path, authStatus, authDetail, RESOLVE_NO_SYMLINKS))
        return fail(authStatus, authDetail);
      if (checkStreamingCancelled(request.requestId, shouldExit, true))
        return shouldExit || cancelled();

      const int destFd = authorizedDir.fd.fd;
      struct stat existing{};
      if (::fstatat(destFd, name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0)
        return fail(AdminStatusCode::AlreadyExists, "An item with that name already exists.");
      if (errno != ENOENT)
        return failErrno(errno);

      const ScopedFd procFd(::open("/proc/self/fd", O_PATH | O_DIRECTORY | O_CLOEXEC));
      struct statfs procFs{};
      if (procFd.fd < 0 || ::fstatfs(procFd.fd, &procFs) != 0 || procFs.f_type != PROC_SUPER_MAGIC)
        return fail(AdminStatusCode::NotSupported, "Copy requires access to the helper's procfs descriptors.");

      AdminStatusCode sourceStatus = AdminStatusCode::AccessDenied;
      bool credentialsRestored = true;
      input = openCopySourceAsInvoker(config.invokingUid, request, procFd.fd, sourceStatus, credentialsRestored);
      if (!credentialsRestored) {
        result.reason = AdminHelperSessionExitReason::IoError;
        result.detail = "Could not restore the helper's filesystem credentials.";
        shouldExit = true;
        return false;
      }
      if (input.fd < 0) {
        if (sourceStatus == AdminStatusCode::NotSupported)
          return fail(sourceStatus, "The source filesystem does not support Administrator Copy.");
        return fail(
            sourceStatus, "Source must be an accessible regular file with an unchanged identity and no symlinks."
        );
      }
      struct stat before{};
      if (::fstat(input.fd, &before) != 0)
        return failErrno(errno);
      if (before.st_size < 0)
        return fail(AdminStatusCode::NotSupported, "The source does not have a valid file size.");

      // An unnamed inode is cleaned up even if the helper dies before the commit.
      stage =
          ScopedFd(openAt2NoMagiclinks(destFd, ".", O_TMPFILE | O_WRONLY, RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS, 0600));
      if (stage.fd < 0) {
        if (errno == EOPNOTSUPP || errno == EISDIR || errno == EINVAL)
          return fail(
              AdminStatusCode::NotSupported, "The destination filesystem does not support private copy staging."
          );
        return failErrno(errno);
      }

      const auto total = static_cast<std::uint64_t>(before.st_size);
      std::uint64_t copied = 0;
      auto lastReport = std::chrono::steady_clock::now();
      const auto progress = [&] {
        return sendServerFrame(
            outFd,
            {
                .kind = AdminServerFrameKind::Progress,
                .requestId = request.requestId,
                .totalItems = 1,
                .completedBytes = copied,
                .totalBytes = total,
                .currentItem = name,
            }
        );
      };
      if (!progress())
        return false;
      std::array<char, 128 * 1024> buffer;
      while (copied < total) {
        if (config.copyCheckpointForTesting)
          config.copyCheckpointForTesting(copied);
        if (checkStreamingCancelled(request.requestId, shouldExit))
          return shouldExit || cancelled();
        const auto length = std::min<std::uint64_t>(buffer.size(), total - copied);
        const ssize_t readBytes = ::read(input.fd, buffer.data(), length);
        if (readBytes < 0 && errno == EINTR)
          continue;
        if (readBytes < 0)
          return failErrno(errno);
        if (readBytes == 0)
          return fail(AdminStatusCode::IdentityMismatch, "The source changed during copy.");
        ssize_t written = 0;
        while (written < readBytes) {
          const ssize_t n = ::write(stage.fd, buffer.data() + written, static_cast<std::size_t>(readBytes - written));
          if (n < 0 && errno == EINTR)
            continue;
          if (n <= 0)
            return failErrno(n < 0 ? errno : EIO);
          written += n;
        }
        copied += static_cast<std::uint64_t>(written);
        const auto now = std::chrono::steady_clock::now();
        if (now - lastReport >= std::chrono::milliseconds(100)) {
          if (!progress())
            return false;
          lastReport = now;
        }
      }

      char extra = 0;
      ssize_t end;
      do {
        end = ::read(input.fd, &extra, 1);
      } while (end < 0 && errno == EINTR);
      if (end < 0)
        return failErrno(errno);
      if (end != 0)
        return fail(AdminStatusCode::IdentityMismatch, "The source changed during copy.");

      // Keep normal rwx and timestamps, with the helper's restricted write mask
      // and no set-id, capabilities or other privileged source metadata.
      const timespec times[]{before.st_atim, before.st_mtim};
      if (::fchmod(stage.fd, before.st_mode & 0755) != 0 || ::futimens(stage.fd, times) != 0 || ::fsync(stage.fd) != 0)
        return failErrno(errno);
      struct stat staged{};
      if (::fstat(stage.fd, &staged) != 0)
        return failErrno(errno);

      if (!progress())
        return false;
      // A parent can be replaced without changing the pinned file's metadata.
      // Recheck the name using O_PATH only before reporting data from this source.
      const bool sourceStillMatches = [&] {
        const ScopedFd currentSource(
            openAt2NoMagiclinks(AT_FDCWD, request.sourcePath.c_str(), O_PATH, RESOLVE_NO_SYMLINKS)
        );
        struct stat after{};
        return currentSource.fd >= 0
            && ::fstat(currentSource.fd, &after) == 0
            && before.st_dev == after.st_dev
            && before.st_ino == after.st_ino
            && before.st_size == after.st_size
            && before.st_ctim.tv_sec == after.st_ctim.tv_sec
            && before.st_ctim.tv_nsec == after.st_ctim.tv_nsec
            && before.st_mtim.tv_sec == after.st_mtim.tv_sec
            && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec;
      }();
      if (!sourceStillMatches)
        return fail(AdminStatusCode::IdentityMismatch, "The source changed during copy.");
      if (!validateAuthorizedDirectoryForWrite(request.path, authStatus, authDetail, RESOLVE_NO_SYMLINKS))
        return fail(authStatus, authDetail);
      if (checkStreamingCancelled(request.requestId, shouldExit))
        return shouldExit || cancelled();
      // linkat publishes exactly the staged inode, atomically and without replacement.
      if (::linkat(procFd.fd, std::to_string(stage.fd).c_str(), destFd, name.c_str(), AT_SYMLINK_FOLLOW) != 0) {
        if (errno == EEXIST)
          return fail(AdminStatusCode::AlreadyExists, "An item with that name already exists.");
        return failErrno(errno);
      }
      stage = ScopedFd{};
      input = ScopedFd{};
      return sendServerFrame(
          outFd,
          {
              .kind = AdminServerFrameKind::MutationResult,
              .requestId = request.requestId,
              .mutationInfo =
                  {name, static_cast<std::uint64_t>(staged.st_dev), static_cast<std::uint64_t>(staged.st_ino)},
          },
          true
      );
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
        lastActivity = std::chrono::steady_clock::now();
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

      // Only meaningful user-initiated requests renew the helper's inactivity clock.
      // Automatic polling, PING keepalives, background statvfs/folder-counts, and cancellation do not.
      const bool renewsActivity = request.command == AdminClientCommand::RenewActivity
          || (request.command == AdminClientCommand::ListDir && request.userInitiated)
          || (request.command == AdminClientCommand::StatInfo && request.userInitiated)
          || request.command == AdminClientCommand::CreateFile
          || request.command == AdminClientCommand::CreateDir
          || request.command == AdminClientCommand::RenameEntry
          || request.command == AdminClientCommand::CopyFile;
      if (renewsActivity)
        lastActivity = std::chrono::steady_clock::now();

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
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - sessionStart)
                .count();
        AdminServerFrame statusFrame;
        statusFrame.kind = AdminServerFrameKind::Status;
        statusFrame.requestId = request.requestId;
        statusFrame.protocolVersion = kAdminProtocolVersion;
        statusFrame.helperPid = helperPid;
        statusFrame.invokingUid = config.invokingUid;
        statusFrame.uptimeMs = elapsedMs > 0 ? static_cast<std::uint64_t>(elapsedMs) : 0ULL;
        statusFrame.activeRequestId = kInvalidAdminRequestId;
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
        result.lastCancelledTargetId = request.targetRequestId;
        if (!sendResponseFrame(outFd, request.requestId, AdminStatusCode::Ok, "Cancellation acknowledged.")) {
          result.reason = AdminHelperSessionExitReason::IoError;
          result.detail = "Failed to write CANCEL response.";
          shouldExit = true;
          return false;
        }
        return true;

      case AdminClientCommand::Shutdown:
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
        if (!executeCopyFile(request, shouldExit) && !shouldExit) {
          result.reason = AdminHelperSessionExitReason::IoError;
          result.detail = "Failed to write COPY_FILE response.";
          shouldExit = true;
        }
        return !shouldExit;

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
      if (remainingTimeMs() == 0)
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
          // A blocked response can consume the remaining inactivity budget.
          if (result.reason == AdminHelperSessionExitReason::IoError)
            (void)remainingTimeMs();
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

      pollfd pfd{inFd, POLLIN, 0};
      const int pollRc = ::poll(&pfd, 1, remainingTimeMs());
      if (pollRc == 0 || (pollRc < 0 && errno == EINTR))
        continue;
      if (pollRc < 0 || (pfd.revents & (POLLERR | POLLNVAL)) != 0) {
        result.reason = AdminHelperSessionExitReason::IoError;
        result.detail = "Could not poll the client channel.";
        return result;
      }

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
