#pragma once

#include <cstdint>
#include <dirent.h>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace filemanager {
  struct FolderSizeRequest {
    std::filesystem::path path;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    bool operator==(const FolderSizeRequest&) const = default;
  };
  struct FolderSizeDiagnostic {
    std::string path; // Relative to the selected root; empty means the root.
    std::string operation;
    int error = 0;
    bool subtree = false;
  };
  struct FolderSizeResult {
    std::uint64_t bytes = 0;
    std::uint64_t files = 0;
    std::uint64_t folders = 0;
    std::uint64_t links = 0;
    std::uint64_t other = 0;
    std::uint64_t errors = 0;
    std::uint64_t excludedMounts = 0;
    std::uint64_t permissionErrors = 0;
    std::uint64_t vanishedEntries = 0;
    std::uint64_t ioErrors = 0;
    std::uint64_t unreadableDirectories = 0;
    bool failed = false; // The selected root could not be opened, verified or enumerated.
    bool cancelled = false;
    std::vector<FolderSizeDiagnostic> diagnostics; // Bounded sample, never an unbounded path queue.
    std::string reason;
    [[nodiscard]] bool partial() const { return failed || cancelled || errors != 0 || excludedMounts != 0; }
  };
  struct FolderSizeOptions {
    std::size_t maxDepth = 128;
    std::uint64_t maxEntries = 1000000;
    // Tests may inject a boundary error; production uses the shared no-follow/no-XDEV opener.
    std::function<int(int, const char*)> openChild;
    // Tests may inject an enumeration failure; production uses readdir.
    std::function<dirent*(DIR*)> readEntry;
  };
  // Read-only, synchronous engine; Properties runs it in one GTask worker.
  // Logical sizes of regular-file and link entries, counting hard links per name.
  // Directory inode sizes and allocated disk usage are not included.
  FolderSizeResult calculateFolderSize(
      const FolderSizeRequest& request, std::function<bool()> cancelled = {}, FolderSizeOptions options = {}
  );
} // namespace filemanager
