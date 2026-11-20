#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

enum class FileDialogSortField : std::uint8_t {
  Name,
  Size,
  Modified,
  Created,
  Accessed,
  Type,
};

enum class FileDialogSortOrder : std::uint8_t {
  Ascending,
  Descending,
};

struct FileEntry {
  std::string name;
  std::filesystem::path absPath;
  bool isDir = false;
  std::uintmax_t size = 0;
  std::filesystem::file_time_type mtime;
  std::uint64_t created = 0;
  std::uint64_t accessed = 0;
  bool hasCreated = false;
  bool hasAccessed = false;
  std::uint64_t device = 0;
  std::uint64_t inode = 0;
  bool hasIdentity = false;
  bool isSymlink = false;
  std::string type;
};

class DirectoryScanner {
public:
  [[nodiscard]] std::vector<FileEntry> scan(
      const std::filesystem::path& dir, const std::vector<std::string>& extensions, bool showHiddenFiles,
      FileDialogSortField sortField, FileDialogSortOrder sortOrder
  ) const;

  [[nodiscard]] static bool isImagePath(const std::filesystem::path& path);
  [[nodiscard]] static bool isSvgPath(const std::filesystem::path& path);
  [[nodiscard]] static bool isThumbnailPath(const std::filesystem::path& path);

  // Extension filter for FileDialogOptions::extensions and
  // TextSetting::browseFileExtensions. `includeSvg` adds ".svg" for pickers
  // that also accept vector art.
  [[nodiscard]] static std::vector<std::string> imageExtensionFilter(bool includeSvg);

private:
  [[nodiscard]] static bool
  matchesExtension(const std::filesystem::path& path, const std::vector<std::string>& extensions);
  [[nodiscard]] static bool isHiddenName(std::string_view name);
  [[nodiscard]] static std::string normalizeExtension(std::string_view extension);
};
