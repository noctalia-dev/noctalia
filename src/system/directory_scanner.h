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
};

class DirectoryScanner {
public:
  std::vector<FileEntry> scan(
      const std::filesystem::path& dir, const std::vector<std::string>& extensions, bool showHiddenFiles,
      FileDialogSortField sortField, FileDialogSortOrder sortOrder
  ) const;

  static bool isImagePath(const std::filesystem::path& path);

  // Extension filter for FileDialogOptions::extensions and
  // TextSetting::browseFileExtensions. `includeSvg` adds ".svg" for pickers
  // that also accept vector art.
  static std::vector<std::string> imageExtensionFilter(bool includeSvg);

private:
  static bool matchesExtension(const std::filesystem::path& path, const std::vector<std::string>& extensions);
  static bool isHiddenName(std::string_view name);
  static std::string normalizeExtension(std::string_view extension);
};
