#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// One concrete icon-theme directory to search, with the metadata needed for
// size-aware selection. size == 0 means the nominal size is unknown.
struct IconSearchDir {
  std::string path;
  int size = 0;
  bool scalable = false;
};

class IconResolver {
public:
  IconResolver();
  explicit IconResolver(bool cacheMissing);

  // targetSize is the intended on-screen pixel size. When > 0, a vector (SVG)
  // icon is preferred and, among bitmaps, the smallest theme size that is still
  // >= targetSize wins (falling back to the largest available) so we downscale
  // gently instead of crushing a 1024px PNG. targetSize == 0 keeps the legacy
  // "prefer scalable, then largest" behavior for callers that have no size.
  const std::string& resolve(const std::string& iconName, int targetSize = 0);
  void invalidateMissingCache();

  static bool checkThemeChanged();
  static std::uint64_t themeGeneration();

private:
  void rebuild();
  void ensureFresh();
  std::string findIcon(const std::string& name, int targetSize) const;

  std::unordered_map<std::string, std::string> m_cache;
  std::unordered_set<std::string> m_missingCache;
  std::vector<std::string> m_baseDirs;     // XDG icon theme roots
  std::vector<IconSearchDir> m_searchDirs; // Ordered list of concrete theme dirs to search
  std::vector<std::string> m_pixmapDirs;   // XDG pixmap fallback roots
  std::string m_empty;
  std::uint64_t m_generation = 0;
  bool m_cacheMissing = false;
};
