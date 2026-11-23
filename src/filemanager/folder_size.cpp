#include "filemanager/folder_size.h"

#include "filemanager/property_operations.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <glib.h>
#include <iomanip>
#include <limits>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace filemanager {
  std::optional<std::uint64_t> countImmediateFolderItems(GFile* directory, GCancellable* cancellable) {
    g_autoptr(GError) error = nullptr;
    g_autoptr(GFileEnumerator) entries = g_file_enumerate_children(
        directory, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, &error
    );
    if (!entries)
      return std::nullopt;
    std::uint64_t count = 0;
    while (auto* child = g_file_enumerator_next_file(entries, cancellable, &error)) {
      g_object_unref(child);
      ++count;
    }
    if (error || g_cancellable_is_cancelled(cancellable))
      return std::nullopt;
    return count;
  }

  FolderSizeResult
  calculateFolderSize(const FolderSizeRequest& request, std::function<bool()> cancelled, FolderSizeOptions options) {
    FolderSizeResult result;
    // The same bounded debug channel carries the terminal state, including
    // early returns and cancellation. No UI path/error dump is needed.
    struct Summary {
      const FolderSizeRequest& request;
      const FolderSizeResult& result;
      ~Summary() {
        gchar* root = g_strescape(request.path.c_str(), nullptr);
        g_debug(
            "Folder size result: root=\"%s\" state=%s files=%" G_GUINT64_FORMAT " folders=%" G_GUINT64_FORMAT
            " links=%" G_GUINT64_FORMAT " other=%" G_GUINT64_FORMAT " bytes=%" G_GUINT64_FORMAT
            " errors=%" G_GUINT64_FORMAT " excluded-mounts=%" G_GUINT64_FORMAT,
            root, folderSizeStatus(result), static_cast<guint64>(result.files), static_cast<guint64>(result.folders),
            static_cast<guint64>(result.links), static_cast<guint64>(result.other), static_cast<guint64>(result.bytes),
            static_cast<guint64>(result.errors), static_cast<guint64>(result.excludedMounts)
        );
        g_free(root);
      }
    } summary{request, result};
    const auto stop = [&] {
      if (cancelled && cancelled())
        result.cancelled = true;
      return result.cancelled;
    };
    const auto diagnose = [&](const std::string& path, const std::string& operation, int error, bool subtree,
                              const std::string& detail = "") {
      if (result.diagnostics.size() < 16) {
        const std::string description = error ? g_strerror(error)
            : detail.empty()                  ? "Traversal safeguard or identity check"
                                              : detail.substr(0, 512);
        result.diagnostics.push_back({path.substr(0, 4096), operation, error, subtree, description});
        gchar* root = g_strescape(request.path.c_str(), nullptr);
        gchar* relative = g_strescape(path.c_str(), nullptr);
        g_debug(
            "Folder size diagnostic: root=\"%s\" path=\"%s\" operation=\"%s\" errno=%d (%s) subtree=%s", root, relative,
            operation.c_str(), error, description.c_str(), subtree ? "yes/possible" : "no"
        );
        g_free(relative);
        g_free(root);
      }
    };
    const auto fail = [&](std::string reason, const std::string& path = "", int error = 0, bool subtree = false,
                          const std::string& operation = "scan") {
      ++result.errors;
      if (error == EACCES || error == EPERM)
        ++result.permissionErrors;
      else if (error == ENOENT || error == ESTALE)
        ++result.vanishedEntries;
      else if (error)
        ++result.ioErrors;
      if (subtree)
        ++result.unreadableDirectories;
      diagnose(path, operation, error, subtree, reason);
      if (result.reason.empty())
        result.reason = reason.substr(0, 512);
    };
    if (stop())
      return result;
    const int root = open(request.path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root < 0) {
      result.failed = true;
      fail(std::strerror(errno), "", errno, true, "open root");
      return result;
    }
    struct CloseRoot {
      int fd;
      ~CloseRoot() { close(fd); }
    } closeRoot{root};
    struct stat rootState{};
    if (fstat(root, &rootState)) {
      result.failed = true;
      fail(std::strerror(errno), "", errno, true, "stat root");
      return result;
    }
    if (static_cast<std::uint64_t>(rootState.st_dev) != request.device || rootState.st_ino != request.inode) {
      result.failed = true;
      fail("The folder was replaced before calculation started.");
      return result;
    }
    struct Frame {
      DIR* directory;
      std::string relative;
      dev_t device;
      ino_t inode;
    };
    std::vector<Frame> stack;
    const auto push = [&](int fd, const std::string& relative, const struct stat& state) {
      const auto pinned = "/proc/self/fd/" + std::to_string(fd);
      const int readable = open(pinned.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      DIR* directory = readable >= 0 ? fdopendir(readable) : nullptr;
      if (!directory) {
        const int error = errno;
        if (readable >= 0)
          close(readable);
        if (relative.empty())
          result.failed = true;
        fail("A directory could not be enumerated.", relative, error, true, "open directory");
        return false;
      }
      stack.push_back({directory, relative, state.st_dev, state.st_ino});
      return true;
    };
    const auto addBytes = [&](off_t bytes) {
      if (bytes < 0 || static_cast<std::uint64_t>(bytes) > std::numeric_limits<std::uint64_t>::max() - result.bytes)
        fail("Logical byte total exceeds the supported range.");
      else
        result.bytes += static_cast<std::uint64_t>(bytes);
    };
    const auto addAllocated = [&](blkcnt_t blocks) {
      if (blocks < 0
          || static_cast<std::uint64_t>(blocks) > std::numeric_limits<std::uint64_t>::max() / 512ULL
          || static_cast<std::uint64_t>(blocks) * 512ULL
              > std::numeric_limits<std::uint64_t>::max() - result.allocatedBytes)
        fail("Allocated byte total exceeds the supported range.");
      else
        result.allocatedBytes += static_cast<std::uint64_t>(blocks) * 512ULL;
    };
    addAllocated(rootState.st_blocks);
    const auto opener = options.openChild ? options.openChild : openPropertyDescendant;
    const auto readEntry = options.readEntry ? options.readEntry : readdir;
    push(root, "", rootState);
    std::uint64_t inspected = 0;
    while (!stack.empty() && !stop()) {
      auto& frame = stack.back();
      errno = 0;
      dirent* entry = readEntry(frame.directory);
      if (!entry) {
        if (errno) {
          const int error = errno;
          if (frame.relative.empty())
            result.failed = true;
          fail("Directory enumeration failed.", frame.relative, error, true, "read directory");
        }
        closedir(frame.directory);
        stack.pop_back();
        continue;
      }
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
        continue;
      if (inspected++ >= options.maxEntries) {
        fail("The entry limit was reached.", frame.relative, 0, true, "entry limit");
        break;
      }
      const std::string relative = frame.relative.empty() ? entry->d_name : frame.relative + "/" + entry->d_name;
      if (relative.size() > 4096) {
        fail("A path exceeded the traversal limit.", relative, 0, true, "path limit");
        continue;
      }
      const int fd = opener(root, relative.c_str());
      const int openError = errno;
      if (stop()) {
        if (fd >= 0)
          close(fd);
        break;
      }
      if (fd < 0) {
        if (openError == EXDEV) {
          ++result.excludedMounts;
          diagnose(relative, "child mount excluded", openError, true);
        } else
          fail(
              "An entry could not be opened.", relative, openError,
              (entry->d_type == DT_DIR || entry->d_type == DT_UNKNOWN), "open entry"
          );
        continue;
      }
      struct stat state{};
      if (fstat(fd, &state))
        fail(
            "An entry could not be inspected.", relative, errno,
            (entry->d_type == DT_DIR || entry->d_type == DT_UNKNOWN), "stat entry"
        );
      else if (state.st_dev != rootState.st_dev) {
        ++result.excludedMounts;
        diagnose(relative, "child mount excluded", EXDEV, S_ISDIR(state.st_mode));
      } else if (S_ISDIR(state.st_mode)) {
        addAllocated(state.st_blocks);
        ++result.folders;
        const bool cycle = std::ranges::any_of(stack, [&](const auto& ancestor) {
          return ancestor.device == state.st_dev && ancestor.inode == state.st_ino;
        });
        if (cycle)
          fail("A directory cycle was excluded.", relative, 0, true, "cycle");
        else if (stack.size() >= options.maxDepth)
          fail("The directory depth limit was reached.", relative, 0, true, "depth limit");
        else
          push(fd, relative, state);
      } else if (S_ISLNK(state.st_mode)) {
        addAllocated(state.st_blocks);
        ++result.links;
        addBytes(state.st_size);
      } else if (S_ISREG(state.st_mode)) {
        addAllocated(state.st_blocks);
        ++result.files;
        addBytes(state.st_size);
      } else {
        addAllocated(state.st_blocks);
        ++result.other;
      }
      close(fd);
    }
    for (const auto& frame : stack)
      closedir(frame.directory);
    if (!result.cancelled) {
      struct stat current{};
      if (lstat(request.path.c_str(), &current))
        fail("Could not verify the folder after calculation.", "", errno, true, "verify root");
      else if (current.st_dev != rootState.st_dev || current.st_ino != rootState.st_ino)
        fail("The folder moved or was replaced during calculation; reopen Properties.");
    }
    return result;
  }

  FolderSizeResult
  calculateSelectionSize(const std::vector<std::filesystem::path>& paths, std::function<bool()> cancelled) {
    FolderSizeResult total;
    const auto stop = [&] {
      if (cancelled && cancelled())
        total.cancelled = true;
      return total.cancelled;
    };
    const auto add = [&](std::uint64_t& target, std::uint64_t value) {
      if (value > std::numeric_limits<std::uint64_t>::max() - target) {
        ++total.errors;
        if (total.reason.empty())
          total.reason = "The combined selection total exceeds the supported range.";
      } else
        target += value;
    };
    const auto merge = [&](const FolderSizeResult& source, const std::filesystem::path& root) {
      add(total.bytes, source.bytes);
      add(total.allocatedBytes, source.allocatedBytes);
      add(total.files, source.files);
      add(total.folders, source.folders);
      add(total.links, source.links);
      add(total.other, source.other);
      add(total.errors, source.errors);
      add(total.excludedMounts, source.excludedMounts);
      add(total.permissionErrors, source.permissionErrors);
      add(total.vanishedEntries, source.vanishedEntries);
      add(total.ioErrors, source.ioErrors);
      add(total.unreadableDirectories, source.unreadableDirectories);
      total.failed = total.failed || source.failed;
      total.cancelled = total.cancelled || source.cancelled;
      total.allocatedSizeAvailable = total.allocatedSizeAvailable && source.allocatedSizeAvailable;
      if (total.reason.empty())
        total.reason = source.reason;
      for (auto diagnostic : source.diagnostics) {
        if (total.diagnostics.size() >= 16)
          break;
        diagnostic.path = root.string() + (diagnostic.path.empty() ? "" : "/" + diagnostic.path);
        total.diagnostics.push_back(std::move(diagnostic));
      }
    };
    for (const auto& path : paths) {
      if (stop())
        break;
      struct stat state{};
      if (lstat(path.c_str(), &state) != 0) {
        total.failed = true;
        ++total.errors;
        if (errno == EACCES || errno == EPERM)
          ++total.permissionErrors;
        else if (errno == ENOENT || errno == ESTALE)
          ++total.vanishedEntries;
        else
          ++total.ioErrors;
        if (total.reason.empty())
          total.reason = std::strerror(errno);
        if (total.diagnostics.size() < 16)
          total.diagnostics.push_back({path.string(), "inspect selected item", errno, true, total.reason});
        continue;
      }
      if (S_ISDIR(state.st_mode)) {
        merge(
            calculateFolderSize(
                {path, static_cast<std::uint64_t>(state.st_dev), static_cast<std::uint64_t>(state.st_ino)}, cancelled
            ),
            path
        );
        continue;
      }
      if (S_ISLNK(state.st_mode))
        ++total.links;
      else if (S_ISREG(state.st_mode))
        ++total.files;
      else
        ++total.other;
      if (state.st_size > 0)
        add(total.bytes, static_cast<std::uint64_t>(state.st_size));
      if (state.st_blocks >= 0
          && static_cast<std::uint64_t>(state.st_blocks) <= std::numeric_limits<std::uint64_t>::max() / 512ULL)
        add(total.allocatedBytes, static_cast<std::uint64_t>(state.st_blocks) * 512ULL);
      else {
        ++total.errors;
        total.allocatedSizeAvailable = false;
        if (total.reason.empty())
          total.reason = "Allocated byte total exceeds the supported range.";
      }
    }
    return total;
  }

  std::string encodeFolderSizeResult(const FolderSizeResult& r) {
    std::ostringstream header;
    header
        << "v2 "
        << r.bytes
        << ' '
        << r.allocatedBytes
        << ' '
        << r.allocatedSizeAvailable
        << ' '
        << r.files
        << ' '
        << r.folders
        << ' '
        << r.links
        << ' '
        << r.other
        << ' '
        << r.errors
        << ' '
        << r.excludedMounts
        << ' '
        << r.permissionErrors
        << ' '
        << r.vanishedEntries
        << ' '
        << r.ioErrors
        << ' '
        << r.unreadableDirectories
        << ' '
        << r.failed
        << ' '
        << r.cancelled
        << ' '
        << std::quoted(r.reason);
    auto encoded = header.str();
    for (const auto& d : r.diagnostics) {
      std::ostringstream entry;
      entry
          << ' '
          << std::quoted(d.path)
          << ' '
          << std::quoted(d.operation)
          << ' '
          << d.error
          << ' '
          << d.subtree
          << ' '
          << std::quoted(d.description);
      auto record = entry.str();
      if (encoded.size() + record.size() <= 4096)
        encoded += record;
    }
    return encoded;
  }

  std::optional<FolderSizeResult> decodeFolderSizeResult(std::string_view encoded) {
    std::istringstream input{std::string(encoded)};
    FolderSizeResult r;
    std::string version;
    if (!(input >> version))
      return std::nullopt;
    if (version == "v2") {
      if (!(input
            >> r.bytes
            >> r.allocatedBytes
            >> r.allocatedSizeAvailable
            >> r.files
            >> r.folders
            >> r.links
            >> r.other
            >> r.errors
            >> r.excludedMounts
            >> r.permissionErrors
            >> r.vanishedEntries
            >> r.ioErrors
            >> r.unreadableDirectories
            >> r.failed
            >> r.cancelled
            >> std::quoted(r.reason)))
        return std::nullopt;
    } else {
      input.clear();
      input.seekg(0);
      r.allocatedSizeAvailable = false;
      if (!(input
            >> r.bytes
            >> r.files
            >> r.folders
            >> r.links
            >> r.other
            >> r.errors
            >> r.excludedMounts
            >> r.permissionErrors
            >> r.vanishedEntries
            >> r.ioErrors
            >> r.unreadableDirectories
            >> r.failed
            >> r.cancelled
            >> std::quoted(r.reason)))
        return std::nullopt;
    }
    while (input >> std::ws && !input.eof()) {
      FolderSizeDiagnostic d;
      if (!(input
            >> std::quoted(d.path)
            >> std::quoted(d.operation)
            >> d.error
            >> d.subtree
            >> std::quoted(d.description)))
        return std::nullopt;
      r.diagnostics.push_back(std::move(d));
    }
    return r;
  }
} // namespace filemanager
