#include "calendar/calendar_cache.h"

#include "config/atomic_file.h"
#include "util/file_utils.h"

namespace calendar::cache {
  bool secureExisting(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::exists(path.parent_path(), ec)
        && !FileUtils::setPrivateDirectoryPermissions(path.parent_path(), ec)) {
      return false;
    }
    if (ec) {
      return false;
    }
    if (std::filesystem::exists(path, ec) && !FileUtils::setPrivateFilePermissions(path, ec)) {
      return false;
    }
    return !ec;
  }

  bool write(const std::filesystem::path& path, std::string_view content) {
    std::error_code ec;
    return FileUtils::createPrivateDirectories(path.parent_path(), ec)
        && writeTextFileAtomic(path, content, FileUtils::privateFileMode());
  }
} // namespace calendar::cache
