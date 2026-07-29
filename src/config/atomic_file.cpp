#include "config/atomic_file.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace {
  [[nodiscard]] mode_t toMode(std::filesystem::perms mode) {
    return static_cast<mode_t>(static_cast<unsigned>(mode & std::filesystem::perms::all));
  }

  void closeFd(int& fd) {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }

  [[nodiscard]] bool writeAll(int fd, std::string_view content) {
    const char* data = content.data();
    std::size_t remaining = content.size();
    while (remaining > 0) {
      const ssize_t written = ::write(fd, data, remaining);
      if (written > 0) {
        data += static_cast<std::size_t>(written);
        remaining -= static_cast<std::size_t>(written);
        continue;
      }
      if (written == 0 || errno != EINTR) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] std::filesystem::path
  resolveRelativeSymlinkTarget(const std::filesystem::path& linkPath, std::filesystem::path target) {
    if (target.is_relative()) {
      target = linkPath.parent_path() / target;
    }
    return target.lexically_normal();
  }
} // namespace

std::optional<AtomicWriteTarget> resolveAtomicWriteTarget(const std::filesystem::path& path) {
  if (path.empty()) {
    return std::nullopt;
  }

  std::error_code ec;
  const auto status = std::filesystem::symlink_status(path, ec);
  if (ec) {
    if (ec == std::errc::no_such_file_or_directory) {
      return AtomicWriteTarget{.path = path, .throughSymlink = false};
    }
    return std::nullopt;
  }

  if (status.type() != std::filesystem::file_type::symlink) {
    return AtomicWriteTarget{.path = path, .throughSymlink = false};
  }

  const auto canonicalTarget = std::filesystem::canonical(path, ec);
  if (!ec) {
    return AtomicWriteTarget{.path = canonicalTarget, .throughSymlink = true};
  }

  ec.clear();
  auto linkTarget = std::filesystem::read_symlink(path, ec);
  if (ec) {
    return std::nullopt;
  }

  return AtomicWriteTarget{.path = resolveRelativeSymlinkTarget(path, std::move(linkTarget)), .throughSymlink = true};
}

bool writeTextFileAtomic(
    const std::filesystem::path& path, std::string_view content, std::optional<std::filesystem::perms> mode
) {
  const auto target = resolveAtomicWriteTarget(path);
  if (!target.has_value() || target->path.empty()) {
    return false;
  }

  std::error_code ec;
  const auto parent = target->path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      return false;
    }
  }

  const std::filesystem::path tmpPath = target->path.string() + ".tmp";
  int fd = ::open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode.has_value() ? toMode(*mode) : 0666);
  if (fd < 0) {
    return false;
  }

  if (mode.has_value() && ::fchmod(fd, toMode(*mode)) != 0) {
    closeFd(fd);
    std::filesystem::remove(tmpPath, ec);
    return false;
  }

  if (!writeAll(fd, content)) {
    closeFd(fd);
    std::filesystem::remove(tmpPath, ec);
    return false;
  }

  if (::close(fd) != 0) {
    fd = -1;
    std::filesystem::remove(tmpPath, ec);
    return false;
  }
  fd = -1;

  std::filesystem::rename(tmpPath, target->path, ec);
  if (ec) {
    std::filesystem::remove(tmpPath, ec);
    return false;
  }
  return true;
}
