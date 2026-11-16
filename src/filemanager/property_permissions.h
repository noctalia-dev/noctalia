#pragma once

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace filemanager {
  struct PermissionResult {
    std::string error;
    mode_t mode = 0;
  };

  // The caller owns the pinned descriptor. Revalidate before modifying it.
  inline PermissionResult
  applyPropertyPermissionsFd(int fd, dev_t device, ino_t inode, mode_t original, mode_t edited) {
    struct stat state{};
    if (fstat(fd, &state) != 0)
      return {std::strerror(errno)};
    if ((!S_ISREG(state.st_mode) && !S_ISDIR(state.st_mode)) || state.st_dev != device || state.st_ino != inode)
      return {"The item was replaced or is no longer a regular file or folder. Reopen Properties."};
    if (geteuid() != 0 && geteuid() != state.st_uid)
      return {"Only the owner can change these permissions."};
    const mode_t changed = (original ^ edited) & 0777;
    const mode_t next = ((state.st_mode & ~changed) | (edited & changed)) & 07777;
    // Linux may silently remove setgid if an unprivileged caller is not a member.
    if ((next & S_ISGID) && geteuid() != 0 && getegid() != state.st_gid) {
      const int count = getgroups(0, nullptr);
      if (count < 0)
        return {std::strerror(errno)};
      std::vector<gid_t> groups(static_cast<std::size_t>(count));
      if (count > 0 && getgroups(count, groups.data()) < 0)
        return {std::strerror(errno)};
      bool member = false;
      for (const auto group : groups)
        member = member || group == state.st_gid;
      if (!member)
        return {"Cannot preserve setgid with the current process privileges."};
    }
    if (changed == 0)
      return {{}, state.st_mode};
    const auto pinned = "/proc/self/fd/" + std::to_string(fd);
    if (chmod(pinned.c_str(), next) != 0)
      return {std::strerror(errno)};
    if (fstat(fd, &state) != 0)
      return {"Permissions were submitted, but could not be verified. Reopen Properties."};
    if ((state.st_mode & 07777) != next)
      return {"The filesystem did not retain all requested permission bits. Reopen Properties.", state.st_mode};
    return {{}, state.st_mode};
  }
  // Pin the inode without requiring read access; never follow a symlink.
  inline PermissionResult applyPropertyPermissions(
      const std::filesystem::path& path, dev_t device, ino_t inode, mode_t original, mode_t edited
  ) {
    const int fd = open(path.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
      return {std::strerror(errno)};
    const auto result = applyPropertyPermissionsFd(fd, device, inode, original, edited);
    close(fd);
    return result;
  }

} // namespace filemanager
