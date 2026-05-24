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
  std::filesystem::file_time_type mtime{};
};

class DirectoryScanner {
public:
  [[nodiscard]] std::vector<FileEntry> scan(
      const std::filesystem::path& dir, const std::vector<std::string>& extensions, bool showHiddenFiles,
      FileDialogSortField sortField, FileDialogSortOrder sortOrder
  ) const;

  [[nodiscard]] static bool isImagePath(const std::filesystem::path& path);

private:
  [[nodiscard]] static bool
  matchesExtension(const std::filesystem::path& path, const std::vector<std::string>& extensions);
  [[nodiscard]] static bool isHiddenName(std::string_view name);
  [[nodiscard]] static std::string normalizeExtension(std::string_view extension);
};
