#include "security/private_files.h"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace security_hw::files {

  bool writeAll(int fd, std::string_view data) {
    while (!data.empty()) {
      const ssize_t n = ::write(fd, data.data(), data.size());
      if (n > 0) {
        data.remove_prefix(static_cast<std::size_t>(n));
        continue;
      }
      if (n == 0 || errno != EINTR) {
        return false;
      }
    }
    return true;
  }

  bool writeNewFile(const std::filesystem::path& path, std::string_view data, mode_t mode) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
    if (fd < 0) {
      return false;
    }
    const bool ok = ::fchmod(fd, mode) == 0 && writeAll(fd, data);
    return ::close(fd) == 0 && ok;
  }

  bool replaceFile(const std::filesystem::path& path, std::string_view text, mode_t mode) {
    std::string tmpl = (path.parent_path() / ("." + path.filename().string() + "-XXXXXX")).string();
    const int fd = ::mkstemp(tmpl.data());
    if (fd < 0) {
      return false;
    }
    bool ok = ::fchmod(fd, mode) == 0 && writeAll(fd, text) && ::fsync(fd) == 0;
    ok = ::close(fd) == 0 && ok;
    std::error_code ec;
    if (ok) {
      std::filesystem::rename(tmpl, path, ec);
      ok = !ec;
    }
    if (!ok) {
      std::filesystem::remove(tmpl, ec);
    }
    return ok;
  }

  std::optional<std::string> readLimited(const std::filesystem::path& path, bool follow, std::size_t limit) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | (follow ? 0 : O_NOFOLLOW));
    if (fd < 0) {
      return std::nullopt;
    }
    std::string data;
    std::array<char, 4096> buffer{};
    while (data.size() <= limit) {
      const ssize_t n = ::read(fd, buffer.data(), buffer.size());
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n == 0) {
        break;
      }
      if (n < 0) {
        ::close(fd);
        return std::nullopt;
      }
      data.append(buffer.data(), static_cast<std::size_t>(n));
    }
    ::close(fd);
    if (data.size() > limit) {
      return std::nullopt;
    }
    return data;
  }

} // namespace security_hw::files
