#include "shell/wallpaper/panel/wallpaper_scanner.h"

#include "core/log.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace {

  constexpr Logger kLog("wp-scan");

  bool hasImageExtension(const std::filesystem::path& p) {
    auto ext = p.extension().string();
    for (char& c : ext) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".webp" || ext == ".bmp" || ext == ".gif";
  }

  void collectFlat(const std::filesystem::path& dir, std::vector<WallpaperEntry>& out) {
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec
         );
         !ec && it != std::filesystem::end(it); it.increment(ec)) {
      if (ec) {
        break;
      }
      const auto& entry = *it;
      if (entry.path().filename().string().starts_with('.')) {
        if (entry.is_directory()) {
          it.disable_recursion_pending();
        }
        continue;
      }

      std::error_code typeEc;
      if (!entry.is_regular_file(typeEc) || typeEc) {
        continue;
      }
      if (!hasImageExtension(entry.path())) {
        continue;
      }
      WallpaperEntry e;
      e.name = entry.path().filename().string();
      e.absPath = entry.path();
      e.isDir = false;
      out.push_back(std::move(e));
    }
  }

  void collectShallow(const std::filesystem::path& dir, std::vector<WallpaperEntry>& out) {
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::directory_iterator(dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
      if (ec) {
        break;
      }
      if (entry.path().filename().string().starts_with('.')) {
        continue;
      }

      std::error_code typeEc;
      if (entry.is_directory(typeEc) && !typeEc) {
        WallpaperEntry e;
        e.name = entry.path().filename().string();
        e.absPath = entry.path();
        e.isDir = true;
        out.push_back(std::move(e));
        continue;
      }
      if (entry.is_regular_file(typeEc) && !typeEc && hasImageExtension(entry.path())) {
        WallpaperEntry e;
        e.name = entry.path().filename().string();
        e.absPath = entry.path();
        e.isDir = false;
        out.push_back(std::move(e));
      }
    }
  }

} // namespace

const WallpaperScanResult& WallpaperScanner::scan(const std::filesystem::path& dir, bool flatten) {
  if (dir.empty()) {
    return m_empty;
  }

  std::error_code ec;
  if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
    return m_empty;
  }

  CacheKey key{dir.string(), flatten};
  const auto mtime = std::filesystem::last_write_time(dir, ec);
  if (ec) {
    return m_empty;
  }

  auto it = m_cache.find(key);
  if (it != m_cache.end() && it->second.dirMtime == mtime) {
    return it->second;
  }

  WallpaperScanResult result;
  result.flatten = flatten;
  result.dirMtime = mtime;
  if (flatten) {
    collectFlat(dir, result.entries);
  } else {
    collectShallow(dir, result.entries);
  }

  // Directories first, then files; both sorted case-insensitively by name.
  std::sort(result.entries.begin(), result.entries.end(), [](const WallpaperEntry& a, const WallpaperEntry& b) {
    if (a.isDir != b.isDir) {
      return a.isDir;
    }
    const auto& as = a.name;
    const auto& bs = b.name;
    for (std::size_t i = 0; i < as.size() && i < bs.size(); ++i) {
      const auto ac = std::tolower(static_cast<unsigned char>(as[i]));
      const auto bc = std::tolower(static_cast<unsigned char>(bs[i]));
      if (ac != bc) {
        return ac < bc;
      }
    }
    return as.size() < bs.size();
  });

  kLog.debug("scanned {} ({}): {} entries", dir.string(), flatten ? "flat" : "shallow", result.entries.size());

  auto [insIt, _] = m_cache.insert_or_assign(std::move(key), std::move(result));
  return insIt->second;
}

void WallpaperScanner::invalidate() { m_cache.clear(); }
