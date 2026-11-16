#include "filemanager/property_operations.h"

#include "filemanager/property_permissions.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <grp.h>
#include <linux/openat2.h>
#include <mutex>
#include <pwd.h>
#include <set>
#include <sstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace filemanager {
  namespace {
    template <class T> bool number(const std::string& text, T& value) {
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
      return !text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
    }
    struct Fd {
      int value = -1;
      explicit Fd(int fd) : value(fd) {}
      ~Fd() {
        if (value >= 0)
          close(value);
      }
      Fd(const Fd&) = delete;
      Fd& operator=(const Fd&) = delete;
    };
    std::mutex leaseMutex;
    std::set<std::pair<std::uint64_t, std::uint64_t>> leases;
    struct Lease {
      std::pair<std::uint64_t, std::uint64_t> key;
      bool acquired;
      Lease(std::uint64_t device, std::uint64_t inode) : key(device, inode) {
        const std::lock_guard lock(leaseMutex);
        acquired = leases.insert(key).second;
      }
      ~Lease() {
        if (acquired) {
          const std::lock_guard lock(leaseMutex);
          leases.erase(key);
        }
      }
    };
    int scopedOpen(int root, const char* path, bool relative) {
      open_how how{};
      how.flags = O_PATH | O_CLOEXEC | O_NOFOLLOW;
      how.resolve = RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
      if (relative)
        how.resolve |= RESOLVE_BENEATH | RESOLVE_NO_XDEV;
      return static_cast<int>(syscall(SYS_openat2, root, path, &how, sizeof(how)));
    }
    std::string pinned(int fd) { return "/proc/self/fd/" + std::to_string(fd); }
  } // namespace

  int openPropertyDescendant(int root, const char* relative) { return scopedOpen(root, relative, true); }

  bool resolvePropertyIdentity(const std::string& name, bool group, std::uint32_t& id) {
    if (name.empty() || name.size() > 256 || name.find('\0') != std::string::npos)
      return false;
    std::vector<char> buffer(16384);
    for (; buffer.size() <= 1024 * 1024; buffer.resize(buffer.size() * 2)) {
      int error;
      if (group) {
        struct group entry{};
        struct group* found = nullptr;
        error = getgrnam_r(name.c_str(), &entry, buffer.data(), buffer.size(), &found);
        if (error == 0) {
          if (!found || found->gr_gid == static_cast<gid_t>(-1))
            return false;
          id = found->gr_gid;
          return true;
        }
      } else {
        passwd entry{};
        passwd* found = nullptr;
        error = getpwnam_r(name.c_str(), &entry, buffer.data(), buffer.size(), &found);
        if (error == 0) {
          if (!found || found->pw_uid == static_cast<uid_t>(-1))
            return false;
          id = found->pw_uid;
          return true;
        }
      }
      if (error != ERANGE)
        return false;
    }
    return false;
  }
  bool propertyGroupEligible(std::uint32_t id) {
    if (geteuid() == 0 || getegid() == id)
      return true;
    const int count = getgroups(0, nullptr);
    if (count < 0)
      return false;
    std::vector<gid_t> groups(static_cast<std::size_t>(count));
    return getgroups(count, groups.data()) >= 0 && std::ranges::find(groups, id) != groups.end();
  }
  PropertyAccounts propertyAccounts() {
    PropertyAccounts result;
    // Enumerate local accounts, then validate selected names through reentrant NSS lookup.
    if (FILE* users = fopen("/etc/passwd", "re")) {
      std::array<char, 65536> buffer{};
      passwd entry{};
      passwd* found = nullptr;
      while (fgetpwent_r(users, &entry, buffer.data(), buffer.size(), &found) == 0 && found)
        result.users.push_back({found->pw_name, found->pw_uid, found->pw_uid == geteuid()});
      fclose(users);
    }
    if (FILE* groups = fopen("/etc/group", "re")) {
      std::array<char, 65536> buffer{};
      struct group entry{};
      struct group* found = nullptr;
      while (fgetgrent_r(groups, &entry, buffer.data(), buffer.size(), &found) == 0 && found)
        result.groups.push_back({found->gr_name, found->gr_gid, propertyGroupEligible(found->gr_gid)});
      fclose(groups);
    }
    std::uint32_t minimum = 1000, maximum = 60000;
    std::ifstream definitions("/etc/login.defs");
    for (std::string line; std::getline(definitions, line);) {
      std::istringstream fields(line);
      std::string key;
      std::uint32_t value;
      if (fields >> key >> value) {
        if (key == "UID_MIN")
          minimum = value;
        if (key == "UID_MAX")
          maximum = value;
      }
    }
    std::ranges::sort(result.users, [=](const auto& a, const auto& b) {
      const auto rank = [=](const auto& entry) {
        return entry.id == geteuid() ? 0 : entry.id >= minimum && entry.id <= maximum ? 1 : 2;
      };
      return rank(a) != rank(b) ? rank(a) < rank(b) : a.name < b.name;
    });
    std::ranges::sort(result.groups, [](const auto& a, const auto& b) {
      if ((a.id == getegid()) != (b.id == getegid()))
        return a.id == getegid();
      return a.eligible != b.eligible ? a.eligible : a.name < b.name;
    });
    return result;
  }
  std::string validatePropertyRequest(const PropertyRequest& request) {
    const auto path = request.path.native();
    if (!request.path.is_absolute()
        || path.size() > 4096
        || path.find('\0') != std::string::npos
        || request.path.lexically_normal() != request.path
        || !request.inode)
      return "Invalid absolute path or file identity.";
    if (request.original > 07777 || request.edited > 07777 || ((request.original ^ request.edited) & ~0777U))
      return "Only standard R/W/X changes are supported.";
    if (request.recursive && (!request.owner.empty() || !request.group.empty()))
      return "Recursive ownership changes are not supported.";
    std::uint32_t id;
    if ((!request.owner.empty() && !resolvePropertyIdentity(request.owner, false, id))
        || (!request.group.empty() && !resolvePropertyIdentity(request.group, true, id)))
      return "The selected account no longer exists.";
    if (request.original == request.edited && request.owner.empty() && request.group.empty())
      return "No changes were requested.";
    return {};
  }
  std::vector<std::string> propertyArguments(const PropertyRequest& r) {
    const auto raw = r.path.native();
    char* path = g_base64_encode(reinterpret_cast<const guchar*>(raw.data()), raw.size());
    std::vector<std::string> result{
        path,
        std::to_string(r.device),
        std::to_string(r.inode),
        std::to_string(r.original),
        std::to_string(r.edited),
        r.owner,
        r.group,
        r.recursive ? "1" : "0"
    };
    g_free(path);
    return result;
  }
  bool parsePropertyArguments(const std::vector<std::string>& args, PropertyRequest& request) {
    if (args.size() != 8 || args[0].empty() || args[0].size() > 5500)
      return false;
    gsize size = 0;
    guchar* decoded = g_base64_decode(args[0].c_str(), &size);
    char* canonical = g_base64_encode(decoded, size);
    const bool valid = args[0] == canonical;
    request = {};
    request.path = std::string(reinterpret_cast<const char*>(decoded), size);
    g_free(decoded);
    g_free(canonical);
    if (!valid
        || !number(args[1], request.device)
        || !number(args[2], request.inode)
        || !number(args[3], request.original)
        || !number(args[4], request.edited)
        || (args[7] != "0" && args[7] != "1"))
      return false;
    request.owner = args[5];
    request.group = args[6];
    request.recursive = args[7] == "1";
    return validatePropertyRequest(request).empty();
  }
  std::string PropertyResult::summary() const {
    return std::string(
               cancelled  ? "Cancelled: "
                   : ok() ? "Completed: "
                          : "Incomplete: "
           )
        + std::to_string(succeeded)
        + " succeeded, "
        + std::to_string(failed)
        + " failed, "
        + std::to_string(skipped)
        + " skipped. "
        + (cancelled ? "Previously applied changes were not rolled back. " : "")
        + (ownershipEffects ? "Ownership changes cleared special bits or file capabilities. " : "")
        + error;
  }

  PropertyResult executePropertyRequest(
      const PropertyRequest& request, PropertyCancelled cancelled, PropertyProgress progress, PropertyLimits limits
  ) {
    PropertyResult result;
    if (const auto error = validatePropertyRequest(request); !error.empty()) {
      result.error = error;
      return result;
    }
    Lease lease(request.device, request.inode);
    if (!lease.acquired) {
      result.error = "An operation on this item is already active.";
      return result;
    }
    const auto stop = [&] {
      if (cancelled && cancelled())
        result.cancelled = true;
      return result.cancelled;
    };
    std::string currentEntry = request.path.filename().native();
    const auto fail = [&](std::string error) {
      ++result.failed;
      if (result.error.empty())
        result.error = currentEntry.substr(0, 1024) + ": " + error;
    };
    if (stop())
      return result;
    Fd root(scopedOpen(AT_FDCWD, request.path.c_str(), false));
    struct stat rootState{};
    if (root.value < 0 || fstat(root.value, &rootState) != 0) {
      if (limits.inspectOnly && (errno == EACCES || errno == EPERM))
        result.authorizationRequired = true;
      fail(std::strerror(errno));
      return result;
    }
    if (static_cast<std::uint64_t>(rootState.st_dev) != request.device
        || rootState.st_ino != request.inode
        || (!S_ISREG(rootState.st_mode) && !S_ISDIR(rootState.st_mode))
        || (request.recursive && !S_ISDIR(rootState.st_mode))) {
      fail("The selected item was replaced or is not a supported file/folder. Reopen Properties.");
      return result;
    }
    // Root-owned helpers also exclude duplicate helper processes through an inode lock.
    Fd lockFd(geteuid() == 0 ? open(pinned(root.value).c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC) : -1);
    if (geteuid() == 0 && (lockFd.value < 0 || flock(lockFd.value, LOCK_EX | LOCK_NB) != 0)) {
      fail("Another operation holds this item, or the item cannot be locked.");
      return result;
    }
    std::uint32_t owner = static_cast<uid_t>(-1), group = static_cast<gid_t>(-1);
    if ((!request.owner.empty() && !resolvePropertyIdentity(request.owner, false, owner))
        || (!request.group.empty() && !resolvePropertyIdentity(request.group, true, group))) {
      fail("Account lookup changed. Reopen Properties.");
      return result;
    }
    const auto apply = [&](int fd, bool selected) {
      if (stop())
        return;
      struct stat state{};
      if (fstat(fd, &state) != 0 || state.st_nlink == 0) {
        fail("An item disappeared.");
        return;
      }
      if (!S_ISREG(state.st_mode) && !S_ISDIR(state.st_mode)) {
        ++result.skipped;
        return;
      }
      if (!selected && S_ISREG(state.st_mode) && state.st_nlink > 1) {
        ++result.skipped;
        return;
      }
      const auto path = pinned(fd);
      if (!selected && state.st_dev != rootState.st_dev) {
        ++result.skipped;
        return;
      }
      if (limits.inspectOnly) {
        ++result.inspected;
        result.authorizationRequired = result.authorizationRequired
            || state.st_uid != geteuid()
            || ((state.st_mode & S_ISGID) && !propertyGroupEligible(state.st_gid));
        if (progress)
          progress(result);
        return;
      }
      if (selected && (!request.owner.empty() || !request.group.empty())) {
        const auto before = state.st_mode;
        const bool caps = getxattr(path.c_str(), "security.capability", nullptr, 0) > 0;
        if (chown(path.c_str(), owner, group) != 0) {
          fail(std::strerror(errno));
          return;
        }
        if (fstat(fd, &state) != 0) {
          fail("Ownership changed, but refresh failed.");
          return;
        }
        result.ownershipEffects = ((before & 06000) & ~state.st_mode) != 0
            || (caps && getxattr(path.c_str(), "security.capability", nullptr, 0) <= 0);
      }
      // Recursive execute edits affect directories only. Existing file execute bits stay intact.
      const auto excluded = !selected && S_ISREG(state.st_mode) ? 0111U : 0U;
      const auto permissions = applyPropertyPermissionsFd(
          fd, state.st_dev, state.st_ino, request.original & ~excluded, request.edited & ~excluded
      );
      if (!permissions.error.empty()) {
        fail(permissions.error);
        return;
      }
      ++result.succeeded;
      if (progress)
        progress(result);
    };
    if (!request.recursive) {
      apply(root.value, true);
      return result;
    }
    struct Frame {
      DIR* directory;
      std::string relative;
      int fd;
    };
    std::vector<Frame> stack;
    const auto push = [&](int fd, std::string relative) {
      const int readable = open(pinned(fd).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      DIR* directory = readable >= 0 ? fdopendir(readable) : nullptr;
      if (!directory) {
        if (limits.inspectOnly && (errno == EACCES || errno == EPERM))
          result.authorizationRequired = true;
        if (readable >= 0)
          close(readable);
        fail(std::strerror(errno));
        close(fd);
        return false;
      }
      stack.push_back({directory, std::move(relative), fd});
      return true;
    };
    push(dup(root.value), "");
    std::uint64_t entries = 0;
    while (!stack.empty() && !stop() && !result.authorizationRequired) {
      auto& frame = stack.back();
      errno = 0;
      dirent* child = readdir(frame.directory);
      if (!child) {
        currentEntry = frame.relative.empty() ? request.path.filename().native() : frame.relative;
        if (errno)
          fail(std::strerror(errno));
        if (frame.relative.empty())
          apply(frame.fd, true);
        else {
          Fd current(openPropertyDescendant(root.value, frame.relative.c_str()));
          struct stat before{}, now{};
          if (current.value < 0
              || fstat(frame.fd, &before)
              || fstat(current.value, &now)
              || before.st_dev != now.st_dev
              || before.st_ino != now.st_ino)
            fail("A directory moved or was replaced during traversal.");
          else
            apply(current.value, false);
        }
        closedir(frame.directory);
        close(frame.fd);
        stack.pop_back();
        continue;
      }
      if (!strcmp(child->d_name, ".") || !strcmp(child->d_name, ".."))
        continue;
      if (++entries > limits.entries) {
        fail("Traversal entry limit reached; remaining contents were not changed.");
        break;
      }
      const auto relative = frame.relative.empty() ? std::string(child->d_name) : frame.relative + "/" + child->d_name;
      currentEntry = relative;
      Fd fd(openPropertyDescendant(root.value, relative.c_str()));
      if (fd.value < 0) {
        if (limits.inspectOnly && (errno == EACCES || errno == EPERM))
          result.authorizationRequired = true;
        if (errno == ELOOP || errno == EXDEV)
          ++result.skipped;
        else
          fail(std::strerror(errno));
        continue;
      }
      struct stat state{};
      if (fstat(fd.value, &state) != 0) {
        fail(std::strerror(errno));
        continue;
      }
      if (S_ISLNK(state.st_mode)) {
        ++result.skipped;
        continue;
      }
      if (S_ISDIR(state.st_mode)) {
        if (stack.size() >= limits.depth || relative.size() > 4096) {
          fail("Traversal depth/path limit reached.");
          continue;
        }
        push(dup(fd.value), relative);
      } else
        apply(fd.value, false);
    }
    for (const auto& frame : stack) {
      closedir(frame.directory);
      close(frame.fd);
    }
    if (progress)
      progress(result);
    return result;
  }
  bool propertyNeedsAuthorization(const PropertyRequest& request, std::uint32_t uid) {
    // One authorisation covers the bounded recursive operation, never one prompt per child.
    return geteuid() != 0 && (uid != geteuid() || !request.owner.empty() || (!request.group.empty() && [&] {
                                std::uint32_t id;
                                return !resolvePropertyIdentity(request.group, true, id) || !propertyGroupEligible(id);
                              }()));
  }
  PropertyResult propertyAuthorizationFailure(int code, bool cancelled) {
    PropertyResult result;
    result.cancelled = cancelled || code == 126;
    result.error = result.cancelled ? "Authorisation cancelled; no new changes will be started."
        : code == 127               ? "Authorisation was denied or unavailable."
                                    : "The authorised helper failed or returned no result.";
    return result;
  }
} // namespace filemanager
