#include "core/files/directory_scanner.h"

#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <numeric>
#include <system_error>

std::vector<FileEntry> DirectoryScanner::scan(
    const std::filesystem::path& dir, const std::vector<std::string>& extensions, bool showHiddenFiles,
    FileDialogSortField sortField, FileDialogSortOrder sortOrder
) const {
  std::vector<FileEntry> entries;

  if (dir.empty()) {
    return entries;
  }

  std::error_code ec;
  if (!std::filesystem::exists(dir, ec) || ec || !std::filesystem::is_directory(dir, ec) || ec) {
    return entries;
  }

  std::vector<std::string> normalizedExtensions;
  normalizedExtensions.reserve(extensions.size());
  for (const auto& extension : extensions) {
    const std::string normalized = normalizeExtension(extension);
    if (!normalized.empty()) {
      normalizedExtensions.push_back(normalized);
    }
  }

  for (const auto& item :
       std::filesystem::directory_iterator(dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
    if (ec) {
      break;
    }

    const std::string name = item.path().filename().string();
    if (!showHiddenFiles && isHiddenName(name)) {
      continue;
    }

    std::error_code typeEc;
    const bool isDir = item.is_directory(typeEc);
    if (typeEc) {
      continue;
    }

    if (!isDir) {
      const bool isRegular = item.is_regular_file(typeEc);
      if (typeEc || !isRegular || !matchesExtension(item.path(), normalizedExtensions)) {
        continue;
      }
    }

    FileEntry entry;
    entry.name = std::move(name);
    entry.absPath = std::filesystem::absolute(item.path(), ec);
    if (ec) {
      entry.absPath = item.path();
      ec.clear();
    }
    entry.isDir = isDir;
    if (!isDir) {
      entry.size = item.file_size(ec);
      if (ec) {
        entry.size = 0;
        ec.clear();
      }
    }
    entry.mtime = item.last_write_time(ec);
    if (ec) {
      entry.mtime = {};
      ec.clear();
    }
    entries.push_back(std::move(entry));
  }

  std::vector<std::string> lowerNames;
  lowerNames.reserve(entries.size());
  for (const auto& entry : entries) {
    lowerNames.push_back(StringUtils::toLower(entry.name));
  }

  std::vector<std::size_t> indices(entries.size());
  std::iota(indices.begin(), indices.end(), std::size_t{0});

  const bool ascending = sortOrder == FileDialogSortOrder::Ascending;
  std::sort(indices.begin(), indices.end(), [&](std::size_t ai, std::size_t bi) {
    const auto& a = entries[ai];
    const auto& b = entries[bi];

    if (a.isDir != b.isDir) {
      return a.isDir > b.isDir;
    }

    switch (sortField) {
    case FileDialogSortField::Size:
      if (!a.isDir && !b.isDir && a.size != b.size) {
        return ascending ? (a.size < b.size) : (a.size > b.size);
      }
      break;
    case FileDialogSortField::Modified:
      if (a.mtime != b.mtime) {
        return ascending ? (a.mtime < b.mtime) : (a.mtime > b.mtime);
      }
      break;
    case FileDialogSortField::Name:
      break;
    }

    const auto& lowerA = lowerNames[ai];
    const auto& lowerB = lowerNames[bi];
    if (lowerA != lowerB) {
      return ascending ? (lowerA < lowerB) : (lowerB < lowerA);
    }
    if (a.name != b.name) {
      return ascending ? (a.name < b.name) : (b.name < a.name);
    }
    return a.absPath.string() < b.absPath.string();
  });

  std::vector<FileEntry> sorted;
  sorted.reserve(entries.size());
  for (std::size_t idx : indices) {
    sorted.push_back(std::move(entries[idx]));
  }
  entries = std::move(sorted);

  return entries;
}

bool DirectoryScanner::isImagePath(const std::filesystem::path& path) {
  static constexpr std::array<std::string_view, 6> kImageExtensions = {
      ".jpg", ".jpeg", ".png", ".webp", ".bmp", ".gif",
  };

  const std::string ext = normalizeExtension(path.extension().string());
  return std::find(kImageExtensions.begin(), kImageExtensions.end(), ext) != kImageExtensions.end();
}

bool DirectoryScanner::matchesExtension(const std::filesystem::path& path, const std::vector<std::string>& extensions) {
  if (extensions.empty()) {
    return true;
  }

  const std::string ext = normalizeExtension(path.extension().string());
  return std::find(extensions.begin(), extensions.end(), ext) != extensions.end();
}

bool DirectoryScanner::isHiddenName(std::string_view name) { return !name.empty() && name.front() == '.'; }

std::string DirectoryScanner::normalizeExtension(std::string_view extension) {
  if (extension.empty()) {
    return {};
  }

  std::string out;
  out.reserve(extension.size() + 1);
  if (extension.front() != '.') {
    out.push_back('.');
  }
  for (char ch : extension) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}
