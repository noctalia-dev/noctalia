#include "util/file_utils.h"

#include "core/log.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  constexpr Logger kLog("download-cache");

  bool secureDirectory(const std::filesystem::path& path) {
    if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
      return false;
    }
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
      return false;
    }
    struct stat info{};
    bool secured = ::fstat(fd, &info) == 0 && info.st_uid == ::geteuid();
    if (secured && (info.st_mode & 07777) != 0700) {
      secured = ::fchmod(fd, 0700) == 0;
    }
    ::close(fd);
    return secured;
  }
} // namespace

std::filesystem::path FileUtils::downloadCacheDir(std::string_view category) {
  if (!category.empty() && (category == "." || category == ".." || category.contains('/'))) {
    return {};
  }
  std::filesystem::path base;
  if (const char* cache = std::getenv("XDG_CACHE_HOME"); cache != nullptr && cache[0] == '/') {
    base = cache;
  } else if (const char* home = std::getenv("HOME"); home != nullptr && home[0] == '/') {
    base = std::filesystem::path(home) / ".cache";
  } else {
    kLog.warn("download cache requires an absolute HOME or XDG_CACHE_HOME");
    return {};
  }

  std::error_code ec;
  std::filesystem::create_directories(base, ec);
  struct stat info{};
  if (ec || ::stat(base.c_str(), &info) != 0 || info.st_uid != ::geteuid() || (info.st_mode & 0022) != 0) {
    kLog.warn("download cache base is unavailable or writable by other users: {}", base.string());
    return {};
  }

  auto directory = base / "noctalia";
  if (!secureDirectory(directory) || !secureDirectory(directory / "downloads")) {
    kLog.warn("could not secure download cache: {}", directory.string());
    return {};
  }
  directory /= "downloads";
  if (!category.empty()) {
    directory /= category;
    if (!secureDirectory(directory)) {
      kLog.warn("could not secure download cache category: {}", directory.string());
      return {};
    }
  }
  return directory;
}
