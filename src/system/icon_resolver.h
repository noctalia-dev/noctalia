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
  std::string theme;
  int minSize = 0;
  int maxSize = 0;
};

struct IconThemeWatchPaths {
  std::vector<std::string> baseDirs;
  std::vector<std::string> themeRoots;
  std::vector<std::string> iconDirs;
  std::vector<std::string> settingsFiles;
};

struct ResolvedIcon {
  std::string path;
  bool symbolic = false;

  bool operator==(const ResolvedIcon&) const = default;
};

class IconResolver {
public:
  IconResolver();
  explicit IconResolver(bool cacheMissing);

  // targetSize is the intended on-screen pixel size. The first theme in
  // inheritance order that has the icon wins. When > 0, a vector (SVG) icon is
  // preferred within that theme and, among its bitmaps, the smallest size that is
  // still >= targetSize wins (falling back to the largest available) so we
  // downscale gently instead of crushing a 1024px PNG. targetSize == 0 keeps the
  // legacy "prefer scalable, then largest" behavior for callers that have no size.
  const std::string& resolve(const std::string& iconName, int targetSize = 0);
  [[nodiscard]] ResolvedIcon resolveSymbolicPreferred(const std::string& iconName, int targetSize = 0);
  // Select status SVG artwork by logical size within the normal winner's theme.
  // Returns empty when normal lookup selects application artwork or a bitmap.
  std::string resolveStatusVector(const std::string& iconName, int targetSize);
  void invalidateMissingCache();

  static bool checkThemeChanged();
  static bool checkThemePlanChanged();
  static void invalidateThemeContent();
  static IconThemeWatchPaths themeWatchPaths();
  static std::uint64_t themeGeneration();
  static std::string activeThemeName();

private:
  void rebuild();
  void ensureFresh();
  std::string findIcon(const std::string& name, int targetSize) const;

  std::unordered_map<std::string, std::string> m_cache;
  std::unordered_map<std::string, ResolvedIcon> m_symbolicPreferredCache;
  std::unordered_set<std::string> m_missingCache;
  std::vector<std::string> m_baseDirs;     // XDG icon base dirs: theme roots and unthemed fallback
  std::vector<IconSearchDir> m_searchDirs; // Ordered list of concrete theme dirs to search
  std::vector<std::string> m_pixmapDirs;   // XDG pixmap fallback roots
  std::string m_empty;
  std::uint64_t m_generation = 0;
  bool m_cacheMissing = false;
};
