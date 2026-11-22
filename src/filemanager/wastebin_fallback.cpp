#include "filemanager/wastebin_fallback.h"

#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <glib.h>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace {

  std::function<std::optional<std::uint64_t>(const char*)> g_availableSpaceTestHook;
  std::function<int(const char*)> g_trashTestHook;

  bool isUnescapedTrashUriByte(unsigned char ch) noexcept {
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))
      return true;
    switch (ch) {
    case '/':
    case '-':
    case '_':
    case '.':
    case '~':
    case '!':
    case '$':
    case '&':
    case '\'':
    case '(':
    case ')':
    case '*':
    case '+':
    case ',':
    case ';':
    case '=':
    case ':':
    case '@':
      return true;
    default:
      return false;
    }
  }

  bool writeAllFd(int fd, const char* data, std::size_t size, int& savedErrno) noexcept {
    std::size_t written = 0;
    while (written < size) {
      const ssize_t n = ::write(fd, data + written, size - written);
      if (n < 0) {
        if (errno == EINTR)
          continue;
        savedErrno = errno;
        return false;
      }
      written += static_cast<std::size_t>(n);
    }
    return true;
  }

  bool ensureOwnedDirectory(const std::filesystem::path& dirPath, GError** error) {
    if (mkdir(dirPath.c_str(), 0700) != 0 && errno != EEXIST) {
      const int saved = errno;
      g_set_error(
          error, G_IO_ERROR, g_io_error_from_errno(saved), "Could not create Home Wastebin directory %s: %s",
          dirPath.c_str(), g_strerror(saved)
      );
      return false;
    }
    struct stat st{};
    if (lstat(dirPath.c_str(), &st) != 0) {
      const int saved = errno;
      g_set_error(
          error, G_IO_ERROR, g_io_error_from_errno(saved), "Could not inspect Home Wastebin directory %s: %s",
          dirPath.c_str(), g_strerror(saved)
      );
      return false;
    }
    if (!S_ISDIR(st.st_mode)) {
      g_set_error(
          error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY, "Home Wastebin path %s is not a directory.", dirPath.c_str()
      );
      return false;
    }
    if (st.st_uid != geteuid()) {
      g_set_error(
          error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
          "Home Wastebin directory %s is not owned by the current user.", dirPath.c_str()
      );
      return false;
    }
    return true;
  }

  bool isComponentSubpath(const std::filesystem::path& candidate, const std::filesystem::path& root) {
    if (candidate.empty() || root.empty())
      return false;
    const auto normCandidate = candidate.lexically_normal();
    const auto normRoot = root.lexically_normal();
    auto rootIt = normRoot.begin();
    auto candIt = normCandidate.begin();
    for (; rootIt != normRoot.end(); ++rootIt, ++candIt) {
      if (rootIt->empty() || *rootIt == ".")
        continue;
      while (candIt != normCandidate.end() && (candIt->empty() || *candIt == "."))
        ++candIt;
      if (candIt == normCandidate.end() || *rootIt != *candIt)
        return false;
    }
    return true;
  }

  bool canonicalOwnedDirectoryPath(
      const std::filesystem::path& rawPath, std::filesystem::path& canonicalOut, std::string& reasonOut,
      const char* label
  ) {
    canonicalOut.clear();
    if (rawPath.empty() || !rawPath.is_absolute()) {
      reasonOut = std::string(label) + " must be an absolute path in test isolation mode.";
      return false;
    }
    std::error_code ec;
    const auto canonical = std::filesystem::canonical(rawPath, ec);
    if (ec || canonical.empty()) {
      reasonOut = std::string(label) + " does not resolve to an existing directory in test isolation mode.";
      return false;
    }
    struct stat st{};
    if (::lstat(canonical.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::geteuid()) {
      reasonOut = std::string(label) + " is not a user-owned directory in test isolation mode.";
      return false;
    }
    canonicalOut = canonical;
    return true;
  }

  bool canonicalOwnedEnvDirectory(const char* envName, std::filesystem::path& canonicalOut, std::string& reasonOut) {
    const char* rawValue = g_getenv(envName);
    if (rawValue == nullptr || *rawValue == '\0') {
      reasonOut = std::string(envName) + " must be set in test isolation mode.";
      return false;
    }
    return canonicalOwnedDirectoryPath(std::filesystem::path(rawValue), canonicalOut, reasonOut, envName);
  }

} // namespace

namespace filemanager::wastebin_fallback {

  bool productionWastebinForbidden() noexcept {
    const char* value = g_getenv("NYTHERA_TEST_FORBID_PRODUCTION_WASTEBIN");
    return value != nullptr && *value != '\0' && g_strcmp0(value, "0") != 0;
  }

  bool validateIsolatedHomeWastebinRoot(const std::filesystem::path& trashRoot, std::string& reasonOut) {
    reasonOut.clear();
    if (trashRoot.empty() || !trashRoot.is_absolute() || trashRoot.filename() != "Trash") {
      reasonOut = "Isolated Home Wastebin root must be an absolute .../Trash path.";
      return false;
    }
    std::filesystem::path fixtureCanon;
    if (!canonicalOwnedEnvDirectory("NYTHERA_TEST_FIXTURE_ROOT", fixtureCanon, reasonOut))
      return false;
    std::filesystem::path parentCanon;
    if (!canonicalOwnedDirectoryPath(trashRoot.parent_path(), parentCanon, reasonOut, "Home Wastebin parent"))
      return false;
    if (!isComponentSubpath(parentCanon, fixtureCanon)) {
      reasonOut = "Refusing Home Wastebin write outside the test fixture root.";
      return false;
    }
    struct stat trashStat{};
    if (::lstat(trashRoot.c_str(), &trashStat) == 0
        && (!S_ISDIR(trashStat.st_mode) || trashStat.st_uid != ::geteuid())) {
      reasonOut = "Isolated Home Wastebin Trash directory is not a user-owned directory.";
      return false;
    }
    return true;
  }

  bool validateIsolatedTrashTarget(const std::filesystem::path& sourcePath, std::string& reasonOut) {
    reasonOut.clear();
    const char* gioVfs = g_getenv("GIO_USE_VFS");
    if (gioVfs == nullptr || g_strcmp0(gioVfs, "local") != 0) {
      reasonOut = "GIO_USE_VFS=local is required before calling g_file_trash() in test isolation mode.";
      return false;
    }
    std::filesystem::path fixtureCanon;
    if (!canonicalOwnedEnvDirectory("NYTHERA_TEST_FIXTURE_ROOT", fixtureCanon, reasonOut))
      return false;
    std::filesystem::path homeCanon;
    if (!canonicalOwnedEnvDirectory("HOME", homeCanon, reasonOut))
      return false;
    if (!isComponentSubpath(homeCanon, fixtureCanon)) {
      reasonOut = "HOME must reside inside NYTHERA_TEST_FIXTURE_ROOT in test isolation mode.";
      return false;
    }
    std::filesystem::path xdgDataCanon;
    if (!canonicalOwnedEnvDirectory("XDG_DATA_HOME", xdgDataCanon, reasonOut))
      return false;
    if (!isComponentSubpath(xdgDataCanon, fixtureCanon)) {
      reasonOut = "XDG_DATA_HOME must reside inside NYTHERA_TEST_FIXTURE_ROOT in test isolation mode.";
      return false;
    }
    if (const char* expectedRoot = g_getenv("NYTHERA_TEST_ISOLATED_TRASH_ROOT");
        expectedRoot != nullptr && *expectedRoot != '\0') {
      std::filesystem::path expectedCanon;
      if (!canonicalOwnedDirectoryPath(
              std::filesystem::path(expectedRoot), expectedCanon, reasonOut, "NYTHERA_TEST_ISOLATED_TRASH_ROOT"
          )) {
        return false;
      }
      if (xdgDataCanon != expectedCanon) {
        reasonOut = "XDG_DATA_HOME does not match NYTHERA_TEST_ISOLATED_TRASH_ROOT.";
        return false;
      }
    }

    if (sourcePath.empty() || !sourcePath.is_absolute()) {
      reasonOut = "Trash target must be an absolute path in test isolation mode.";
      return false;
    }
    const auto name = sourcePath.filename();
    if (name.empty() || name == "." || name == "..") {
      reasonOut = "Trash target must have a valid entry filename.";
      return false;
    }
    std::filesystem::path parentCanon;
    if (!canonicalOwnedDirectoryPath(sourcePath.parent_path(), parentCanon, reasonOut, "Trash target parent"))
      return false;
    if (!isComponentSubpath(parentCanon, fixtureCanon)) {
      reasonOut = "Refusing unhooked trash() outside the isolated test fixture root.";
      return false;
    }

    struct stat srcStat{};
    if (::lstat(sourcePath.c_str(), &srcStat) != 0) {
      reasonOut = "Trash target does not exist inside the test fixture.";
      return false;
    }
    struct stat homeStat{};
    struct stat xdgStat{};
    if (::stat(homeCanon.c_str(), &homeStat) != 0 || ::stat(xdgDataCanon.c_str(), &xdgStat) != 0) {
      reasonOut = "Could not verify isolated trash filesystem device.";
      return false;
    }
    if (srcStat.st_dev != homeStat.st_dev || srcStat.st_dev != xdgStat.st_dev) {
      reasonOut = "Refusing cross-filesystem g_file_trash() that could write to a mount Wastebin.";
      return false;
    }
    return true;
  }

  std::filesystem::path homeWastebinRoot() {
    if (const char* xdgDataHome = g_getenv("XDG_DATA_HOME"); xdgDataHome != nullptr && *xdgDataHome != '\0')
      return std::filesystem::path(xdgDataHome) / "Trash";
    if (const char* userDataDir = g_get_user_data_dir(); userDataDir != nullptr && *userDataDir != '\0')
      return std::filesystem::path(userDataDir) / "Trash";
    return std::filesystem::path(g_get_home_dir()) / ".local" / "share" / "Trash";
  }

  std::filesystem::path mountRootForPath(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path current = std::filesystem::absolute(path, ec).lexically_normal();
    if (ec || current.empty())
      current = path;

    struct stat currentStat{};
    while (!current.empty() && lstat(current.c_str(), &currentStat) != 0) {
      const auto parent = current.parent_path();
      if (parent == current)
        break;
      current = parent;
    }
    if (current.empty())
      return "/";
    if (!S_ISDIR(currentStat.st_mode)) {
      const auto parent = current.parent_path();
      if (!parent.empty() && lstat(parent.c_str(), &currentStat) == 0)
        current = parent;
    }

    const dev_t targetDev = currentStat.st_dev;
    while (true) {
      const auto parent = current.parent_path();
      if (parent.empty() || parent == current)
        break;
      struct stat parentStat{};
      if (lstat(parent.c_str(), &parentStat) != 0 || parentStat.st_dev != targetDev)
        break;
      current = parent;
    }
    return current;
  }

  TrashFailureDiagnosis diagnoseTrashFailure(const std::filesystem::path& sourcePath, const GError* gioError) {
    TrashFailureDiagnosis diag;
    diag.technicalDetail = gioError != nullptr && gioError->message != nullptr ? gioError->message : "Trash failed.";

    struct stat sourceStat{};
    if (lstat(sourcePath.c_str(), &sourceStat) != 0) {
      diag.summary = diag.technicalDetail;
      return diag;
    }

    const std::filesystem::path parentPath = sourcePath.parent_path();
    if (parentPath.empty() || access(parentPath.c_str(), W_OK | X_OK) != 0) {
      const int accessErr = errno;
      if (accessErr == EROFS)
        diag.summary = "The item is on a read-only filesystem and cannot be moved to Wastebin.";
      else if (accessErr == EACCES || accessErr == EPERM)
        diag.summary = "You do not have permission to remove items from " + parentPath.string() + ".";
      else
        diag.summary = diag.technicalDetail;
      return diag;
    }

    struct statvfs parentVfs{};
    if (statvfs(parentPath.c_str(), &parentVfs) == 0 && (parentVfs.f_flag & ST_RDONLY) != 0) {
      diag.summary = "The item is on a read-only filesystem and cannot be moved to Wastebin.";
      return diag;
    }

    if (gioError != nullptr
        && (g_error_matches(gioError, G_IO_ERROR, G_IO_ERROR_NO_SPACE)
            || g_error_matches(gioError, G_IO_ERROR, G_IO_ERROR_CANCELLED)
            || g_error_matches(gioError, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))) {
      diag.summary = diag.technicalDetail;
      return diag;
    }

    const std::filesystem::path homeTrash = homeWastebinRoot();
    const std::filesystem::path homeCompare = std::filesystem::exists(homeTrash) ? homeTrash : homeTrash.parent_path();
    struct stat homeStat{};
    const bool haveHomeDev = lstat(homeCompare.c_str(), &homeStat) == 0;
    const bool crossFilesystem = !haveHomeDev || homeStat.st_dev != sourceStat.st_dev;

    diag.mountRoot = mountRootForPath(sourcePath);
    const uid_t euid = geteuid();
    const std::filesystem::path uidTrashDir =
        diag.mountRoot / (".Trash-" + std::to_string(static_cast<unsigned long>(euid)));
    const std::filesystem::path sharedTrashRoot = diag.mountRoot / ".Trash";
    const std::filesystem::path sharedUidTrashDir = sharedTrashRoot / std::to_string(static_cast<unsigned long>(euid));

    struct stat trashStat{};
    if (lstat(uidTrashDir.c_str(), &trashStat) == 0) {
      diag.trashDirPath = uidTrashDir;
      if (!S_ISDIR(trashStat.st_mode)) {
        diag.eligibleForHomeFallback = crossFilesystem;
        diag.summary = "The mount Wastebin path "
            + uidTrashDir.string()
            + " is not a regular directory (symbolic links and non-directories are rejected for security).";
        return diag;
      }
      if (trashStat.st_uid != euid) {
        diag.eligibleForHomeFallback = crossFilesystem;
        diag.summary = "The mount Wastebin directory "
            + uidTrashDir.string()
            + " is owned by UID "
            + std::to_string(static_cast<unsigned long>(trashStat.st_uid))
            + " instead of your user UID "
            + std::to_string(static_cast<unsigned long>(euid))
            + ", so the FreeDesktop Trash ownership check rejected it.";
        return diag;
      }
      if (access(uidTrashDir.c_str(), W_OK | X_OK) != 0) {
        diag.eligibleForHomeFallback = crossFilesystem;
        diag.summary =
            "The mount Wastebin directory " + uidTrashDir.string() + " is not writable by your user account.";
        return diag;
      }
    } else {
      diag.trashDirPath = uidTrashDir;
      struct stat sharedStat{};
      if (lstat(sharedTrashRoot.c_str(), &sharedStat) == 0) {
        if (!S_ISDIR(sharedStat.st_mode) || (sharedStat.st_mode & S_ISVTX) == 0) {
          if (access(diag.mountRoot.c_str(), W_OK | X_OK) != 0) {
            diag.eligibleForHomeFallback = crossFilesystem;
            diag.summary = "The shared Wastebin directory "
                + sharedTrashRoot.string()
                + " lacks the required sticky bit and "
                + uidTrashDir.string()
                + " could not be created on "
                + diag.mountRoot.string()
                + ".";
            return diag;
          }
        } else {
          struct stat subStat{};
          if (lstat(sharedUidTrashDir.c_str(), &subStat) == 0 && subStat.st_uid != euid) {
            diag.trashDirPath = sharedUidTrashDir;
            diag.eligibleForHomeFallback = crossFilesystem;
            diag.summary = "The mount Wastebin directory "
                + sharedUidTrashDir.string()
                + " is owned by UID "
                + std::to_string(static_cast<unsigned long>(subStat.st_uid))
                + " instead of your user UID "
                + std::to_string(static_cast<unsigned long>(euid))
                + ".";
            return diag;
          }
        }
      }
      if (access(diag.mountRoot.c_str(), W_OK | X_OK) != 0) {
        diag.eligibleForHomeFallback = crossFilesystem;
        diag.summary = "Cannot create the mount Wastebin directory "
            + uidTrashDir.string()
            + " because the mount root "
            + diag.mountRoot.string()
            + " is not writable.";
        return diag;
      }
    }

    if (crossFilesystem
        && (gioError == nullptr
            || g_error_matches(gioError, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)
            || g_error_matches(gioError, G_IO_ERROR, G_IO_ERROR_FAILED)
            || g_error_matches(gioError, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))) {
      diag.eligibleForHomeFallback = true;
      diag.summary = "The filesystem at "
          + diag.mountRoot.string()
          + " could not open or create a per-mount Wastebin ("
          + uidTrashDir.string()
          + "): "
          + diag.technicalDetail;
      return diag;
    }

    diag.summary = diag.technicalDetail;
    return diag;
  }

  bool ensureHomeWastebinDirs(
      std::filesystem::path& rootOut, std::filesystem::path& infoDirOut, std::filesystem::path& filesDirOut,
      GError** error
  ) {
    rootOut = homeWastebinRoot();
    if (productionWastebinForbidden()) {
      std::string reason;
      if (!validateIsolatedHomeWastebinRoot(rootOut, reason)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, reason.c_str());
        return false;
      }
    }
    std::error_code ec;
    const auto parent = rootOut.parent_path();
    if (!parent.empty())
      std::filesystem::create_directories(parent, ec);
    if (!ensureOwnedDirectory(rootOut, error))
      return false;
    infoDirOut = rootOut / "info";
    filesDirOut = rootOut / "files";
    if (!ensureOwnedDirectory(infoDirOut, error))
      return false;
    if (!ensureOwnedDirectory(filesDirOut, error))
      return false;
    return true;
  }

  std::string encodeTrashInfoPath(const std::filesystem::path& absolutePath) {
    const std::string raw = absolutePath.string();
    std::string out;
    out.reserve(raw.size());
    constexpr char hex[] = "0123456789ABCDEF";
    for (const unsigned char ch : raw) {
      if (isUnescapedTrashUriByte(ch)) {
        out.push_back(static_cast<char>(ch));
      } else {
        out.push_back('%');
        out.push_back(hex[(ch >> 4) & 0x0F]);
        out.push_back(hex[ch & 0x0F]);
      }
    }
    return out;
  }

  std::string formatTrashDeletionDate(std::time_t timestamp) {
    std::tm localTm{};
    localtime_r(&timestamp, &localTm);
    char buf[32]{};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &localTm);
    return buf;
  }

  bool
  reserveHomeTrashEntry(const std::filesystem::path& originalAbsolutePath, ReservedTrashEntry& out, GError** error) {
    std::filesystem::path rootDir;
    std::filesystem::path infoDir;
    std::filesystem::path filesDir;
    if (!ensureHomeWastebinDirs(rootDir, infoDir, filesDir, error))
      return false;

    const std::filesystem::path filename = originalAbsolutePath.filename();
    const std::string baseName = filename.empty() ? "item" : filename.string();
    const auto dotPos = baseName.find('.', 1);
    const std::string stem = dotPos == std::string::npos ? baseName : baseName.substr(0, dotPos);
    const std::string ext = dotPos == std::string::npos ? std::string{} : baseName.substr(dotPos);

    const std::string encodedPath = encodeTrashInfoPath(originalAbsolutePath);
    const std::string deletionDate = formatTrashDeletionDate(std::time(nullptr));
    const std::string payload = "[Trash Info]\nPath=" + encodedPath + "\nDeletionDate=" + deletionDate + "\n";

    const int infoDirFd = ::open(infoDir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (infoDirFd < 0) {
      const int saved = errno;
      g_set_error(
          error, G_IO_ERROR, g_io_error_from_errno(saved), "Could not open Home Wastebin info directory: %s",
          g_strerror(saved)
      );
      return false;
    }
    const int filesDirFd = ::open(filesDir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (filesDirFd < 0) {
      const int saved = errno;
      close(infoDirFd);
      g_set_error(
          error, G_IO_ERROR, g_io_error_from_errno(saved), "Could not open Home Wastebin files directory: %s",
          g_strerror(saved)
      );
      return false;
    }

    for (unsigned suffix = 1; suffix < 10000; ++suffix) {
      std::string candidate = suffix == 1 ? baseName : (stem + "." + std::to_string(suffix) + ext);
      if (suffix == 9999) {
        g_autofree char* uuid = g_uuid_string_random();
        candidate = stem + "." + uuid + ext;
      }
      struct stat existingFileStat{};
      if (fstatat(filesDirFd, candidate.c_str(), &existingFileStat, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT)
        continue;

      const std::string infoLeaf = candidate + ".trashinfo";
      const int infoFd =
          openat(infoDirFd, infoLeaf.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
      if (infoFd < 0) {
        if (errno == EEXIST)
          continue;
        const int saved = errno;
        close(filesDirFd);
        close(infoDirFd);
        g_set_error(
            error, G_IO_ERROR, g_io_error_from_errno(saved), "Could not create Home Wastebin metadata file: %s",
            g_strerror(saved)
        );
        return false;
      }

      int writeErr = 0;
      const bool wrote = writeAllFd(infoFd, payload.data(), payload.size(), writeErr);
      const int closeRes = close(infoFd);
      if (!wrote || closeRes != 0) {
        const int saved = !wrote ? writeErr : errno;
        unlinkat(infoDirFd, infoLeaf.c_str(), 0);
        close(filesDirFd);
        close(infoDirFd);
        g_set_error(
            error, G_IO_ERROR, g_io_error_from_errno(saved ? saved : EIO),
            "Could not write Home Wastebin metadata file: %s", g_strerror(saved ? saved : EIO)
        );
        return false;
      }

      // Verify no orphan appeared in Trash/files/<candidate> after reserving the .trashinfo file.
      if (fstatat(filesDirFd, candidate.c_str(), &existingFileStat, AT_SYMLINK_NOFOLLOW) == 0) {
        unlinkat(infoDirFd, infoLeaf.c_str(), 0);
        continue;
      }

      close(filesDirFd);
      close(infoDirFd);
      out = ReservedTrashEntry{
          .trashName = candidate,
          .infoPath = infoDir / infoLeaf,
          .filesPath = filesDir / candidate,
      };
      return true;
    }

    close(filesDirFd);
    close(infoDirFd);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "Could not allocate a unique name in the Home Wastebin.");
    return false;
  }

  std::optional<std::uint64_t> queryAvailableBytes(const std::filesystem::path& path) {
    if (g_availableSpaceTestHook)
      return g_availableSpaceTestHook(path.c_str());

    std::filesystem::path probe = path;
    while (!probe.empty()) {
      struct statvfs vfs{};
      if (statvfs(probe.c_str(), &vfs) == 0) {
        const std::uint64_t blockSize = vfs.f_frsize != 0 ? vfs.f_frsize : vfs.f_bsize;
        return static_cast<std::uint64_t>(vfs.f_bavail) * blockSize;
      }
      const auto parent = probe.parent_path();
      if (parent == probe)
        break;
      probe = parent;
    }
    return std::nullopt;
  }

  std::string formatByteSize(std::uint64_t bytes) {
    constexpr std::array<const char*, 6> units{"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double value = static_cast<double>(bytes);
    std::size_t unitIndex = 0;
    while (value >= 1024.0 && unitIndex + 1 < units.size()) {
      value /= 1024.0;
      ++unitIndex;
    }
    std::ostringstream oss;
    if (unitIndex == 0 || value >= 10.0)
      oss << static_cast<std::uint64_t>(std::llround(value)) << ' ' << units[unitIndex];
    else
      oss << std::fixed << std::setprecision(1) << value << ' ' << units[unitIndex];
    return oss.str();
  }

  int invokeTrashHook(const char* sourcePath) noexcept {
    if (!g_trashTestHook)
      return 0;
    return g_trashTestHook(sourcePath);
  }

} // namespace filemanager::wastebin_fallback

void setWastebinAvailableSpaceHookForTesting(std::function<std::optional<std::uint64_t>(const char*)> hook) {
  g_availableSpaceTestHook = std::move(hook);
}

void setFileOperationTrashHookForTesting(std::function<int(const char*)> hook) { g_trashTestHook = std::move(hook); }
