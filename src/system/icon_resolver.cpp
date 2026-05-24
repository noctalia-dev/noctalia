#include "system/icon_resolver.h"

#include "util/string_utils.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gio/gio.h>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

namespace fs = std::filesystem;

namespace {

  struct IconThemePlan {
    std::vector<std::string> baseDirs;
    std::vector<IconSearchDir> searchDirs;
    std::vector<std::string> pixmapDirs;
    std::string activeTheme;
    std::string signature;
  };

  void pushUniqueDir(std::vector<IconSearchDir>& dirs, IconSearchDir dir) {
    if (dir.path.empty()) {
      return;
    }
    if (std::find_if(dirs.begin(), dirs.end(), [&](const IconSearchDir& d) { return d.path == dir.path; }) ==
        dirs.end()) {
      dirs.push_back(std::move(dir));
    }
  }

  // Nominal size encoded in a well-known subdir name like "48x48" or "scalable".
  int sizeFromDirName(std::string_view dirName) {
    if (dirName.find("scalable") != std::string_view::npos) {
      return 0;
    }
    int size = 0;
    for (char c : dirName) {
      if (c >= '0' && c <= '9') {
        size = size * 10 + (c - '0');
      } else if (size > 0) {
        break;
      }
    }
    return size;
  }

  struct IconThemeState {
    bool initialized = false;
    std::uint64_t generation = 1;
    IconThemePlan plan;
  };

  IconThemeState& iconThemeState() {
    static IconThemeState state;
    return state;
  }

  std::string trimAndUnquote(std::string_view value) { return StringUtils::unquote(StringUtils::trim(value)); }

  void pushUnique(std::vector<std::string>& values, std::string value) {
    if (value.empty()) {
      return;
    }
    if (std::find(values.begin(), values.end(), value) == values.end()) {
      values.push_back(std::move(value));
    }
  }

  std::vector<std::string> splitList(std::string_view value, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= value.size()) {
      const auto next = value.find(separator, start);
      const auto part = next == std::string_view::npos ? value.substr(start) : value.substr(start, next - start);
      const std::string trimmed = trimAndUnquote(part);
      if (!trimmed.empty()) {
        parts.push_back(trimmed);
      }
      if (next == std::string_view::npos) {
        break;
      }
      start = next + 1;
    }
    return parts;
  }

  std::vector<std::string> xdgDataDirs() {
    std::vector<std::string> dirs;

    const char* dataHome = std::getenv("XDG_DATA_HOME");
    if (dataHome != nullptr && dataHome[0] != '\0') {
      pushUnique(dirs, dataHome);
    } else if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
      pushUnique(dirs, std::string(home) + "/.local/share");
    }

    const char* dataDirs = std::getenv("XDG_DATA_DIRS");
    if (dataDirs != nullptr && dataDirs[0] != '\0') {
      for (auto dir : splitList(dataDirs, ':')) {
        pushUnique(dirs, std::move(dir));
      }
    } else {
      pushUnique(dirs, "/usr/local/share");
      pushUnique(dirs, "/usr/share");
    }

    return dirs;
  }

  std::vector<std::string> iconBaseDirs(const std::vector<std::string>& dataDirs) {
    std::vector<std::string> roots;
    if (!dataDirs.empty()) {
      pushUnique(roots, dataDirs.front() + "/icons");
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
      pushUnique(roots, std::string(home) + "/.icons");
    }
    for (std::size_t i = dataDirs.empty() ? 0 : 1; i < dataDirs.size(); ++i) {
      pushUnique(roots, dataDirs[i] + "/icons");
    }
    return roots;
  }

  std::vector<std::string> pixmapDirs(const std::vector<std::string>& dataDirs) {
    std::vector<std::string> roots;
    for (const auto& dataDir : dataDirs) {
      pushUnique(roots, dataDir + "/pixmaps");
    }
    return roots;
  }

  std::string signatureFor(const IconThemePlan& plan) {
    std::string signature = plan.activeTheme;
    signature += '\n';
    for (const auto& root : plan.baseDirs) {
      signature += "root:";
      signature += root;
      signature += '\n';
    }
    for (const auto& dir : plan.searchDirs) {
      signature += "theme:";
      signature += dir.path;
      signature += '\n';
    }
    for (const auto& dir : plan.pixmapDirs) {
      signature += "pixmap:";
      signature += dir;
      signature += '\n';
    }
    return signature;
  }

  struct GSettingsDeleter {
    void operator()(GSettings* settings) const {
      if (settings != nullptr) {
        g_object_unref(settings);
      }
    }
  };

  GSettings* iconSettings() {
    static std::unique_ptr<GSettings, GSettingsDeleter> settings = []() {
      GSettingsSchemaSource* source = g_settings_schema_source_get_default();
      if (source == nullptr) {
        return std::unique_ptr<GSettings, GSettingsDeleter>{nullptr};
      }

      GSettingsSchema* schema = g_settings_schema_source_lookup(source, "org.gnome.desktop.interface", TRUE);
      if (schema == nullptr) {
        return std::unique_ptr<GSettings, GSettingsDeleter>{nullptr};
      }

      const bool hasIconThemeKey = g_settings_schema_has_key(schema, "icon-theme") != FALSE;
      g_settings_schema_unref(schema);
      if (!hasIconThemeKey) {
        return std::unique_ptr<GSettings, GSettingsDeleter>{nullptr};
      }

      return std::unique_ptr<GSettings, GSettingsDeleter>{g_settings_new("org.gnome.desktop.interface")};
    }();
    return settings.get();
  }

  std::optional<std::string> readGSettingsIconTheme() {
    GSettings* settings = iconSettings();
    if (settings == nullptr) {
      return std::nullopt;
    }

    gchar* raw = g_settings_get_string(settings, "icon-theme");
    if (raw == nullptr) {
      return std::nullopt;
    }

    std::string value = trimAndUnquote(raw);
    g_free(raw);
    if (value.empty()) {
      return std::nullopt;
    }
    return value;
  }

  // Returns theme name candidates in priority order:
  //   1. GSettings (the canonical source used by compositors and other launchers)
  //   2. GTK3 ini file (reliable fallback)
  //   3. GTK4 ini file (may be stale)
  std::vector<std::string> readGtkThemeCandidates() {
    std::vector<std::string> candidates;

    if (auto value = readGSettingsIconTheme(); value.has_value()) {
      candidates.emplace_back(std::move(*value));
    }

    // 2. GTK ini files
    const char* home = std::getenv("HOME");
    if (home != nullptr) {
      for (const char* cfg : {"/.config/gtk-3.0/settings.ini", "/.config/gtk-4.0/settings.ini"}) {
        std::ifstream file(std::string(home) + cfg);
        if (!file.is_open()) {
          continue;
        }
        std::string line;
        while (std::getline(file, line)) {
          if (!line.starts_with("gtk-icon-theme-name")) {
            continue;
          }
          auto eq = line.find('=');
          if (eq == std::string::npos) {
            continue;
          }
          std::string value = trimAndUnquote(std::string_view(line.data() + eq + 1, line.size() - eq - 1));
          if (!value.empty()) {
            candidates.emplace_back(std::move(value));
          }
        }
      }
    }

    return candidates;
  }

  // Parse index.theme and return (subdir paths sorted by preference, parent theme names).
  // Preference: scalable/large dirs first so we get crisp icons at any size.
  std::pair<std::vector<IconSearchDir>, std::vector<std::string>> parseIndexTheme(const std::string& themeRoot) {
    std::ifstream file(themeRoot + "/index.theme");
    if (!file.is_open()) {
      return {};
    }

    struct DirEntry {
      std::string path;
      int size = 0;
      int maxSize = 0;
      bool scalable = false;
    };

    std::vector<std::string> dirNames;
    std::vector<std::string> inherits;
    std::unordered_map<std::string, DirEntry> dirMap;

    std::string currentSection;
    std::string line;
    while (std::getline(file, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
      }
      if (line.empty() || line[0] == '#') {
        continue;
      }
      if (line[0] == '[') {
        currentSection = line.substr(1, line.size() - 2);
        continue;
      }
      auto eq = line.find('=');
      if (eq == std::string::npos) {
        continue;
      }
      std::string_view key(line.data(), eq);
      std::string_view value(line.data() + eq + 1, line.size() - eq - 1);

      if (currentSection == "Icon Theme") {
        if (key == "Directories") {
          for (auto name : splitList(value, ',')) {
            dirNames.emplace_back(name);
            dirMap[std::move(name)].path = dirNames.back();
          }
        } else if (key == "Inherits") {
          for (auto name : splitList(value, ',')) {
            inherits.emplace_back(std::move(name));
          }
        }
      } else if (!currentSection.empty() && dirMap.count(currentSection)) {
        auto& entry = dirMap[currentSection];
        if (key == "Size") {
          try {
            entry.size = std::stoi(std::string(value));
          } catch (...) {
          }
        } else if (key == "Type") {
          entry.scalable = (value == "Scalable" || value == "Threshold");
        } else if (key == "MaxSize") {
          // For threshold/scalable dirs, MaxSize gives a better sense of actual size
          try {
            entry.maxSize = std::stoi(std::string(value));
          } catch (...) {
          }
        }
      }
    }

    // Sort dirs: scalable first, then by size descending (MaxSize first, then use Size as a tiebreaker)
    std::stable_sort(dirNames.begin(), dirNames.end(), [&](const std::string& a, const std::string& b) {
      const auto& da = dirMap[a];
      const auto& db = dirMap[b];
      if (da.scalable != db.scalable)
        return da.scalable > db.scalable;
      if (da.maxSize != db.maxSize)
        return da.maxSize > db.maxSize;
      return da.size > db.size;
    });

    std::vector<IconSearchDir> sortedPaths;
    sortedPaths.reserve(dirNames.size());
    for (const auto& name : dirNames) {
      const auto& entry = dirMap[name];
      sortedPaths.push_back(IconSearchDir{.path = name, .size = entry.size, .scalable = entry.scalable});
    }

    return {sortedPaths, inherits};
  }

  void buildThemeSearchPaths(
      const std::string& themeName, const std::vector<std::string>& baseDirs, std::set<std::string>& visited,
      std::vector<IconSearchDir>& searchDirs
  ) {
    if (visited.count(themeName)) {
      return;
    }
    visited.insert(themeName);

    for (const auto& base : baseDirs) {
      const std::string themeRoot = base + "/" + themeName;
      if (!fs::is_directory(themeRoot)) {
        continue;
      }

      auto [dirs, inherits] = parseIndexTheme(themeRoot);

      if (dirs.empty()) {
        // No index.theme — fall back to common paths so the theme isn't silently skipped
        for (const char* path :
             {"/scalable/apps/", "/256x256/apps/", "/128x128/apps/", "/64x64/apps/", "/48x48/apps/", "/32x32/apps/"}) {
          const std::string_view name(path);
          pushUniqueDir(
              searchDirs, IconSearchDir{
                              .path = themeRoot + path,
                              .size = sizeFromDirName(name),
                              .scalable = name.find("scalable") != std::string_view::npos
                          }
          );
        }
      } else {
        for (const auto& dir : dirs) {
          pushUniqueDir(
              searchDirs,
              IconSearchDir{.path = themeRoot + "/" + dir.path + "/", .size = dir.size, .scalable = dir.scalable}
          );
        }
      }

      for (const auto& parent : inherits) {
        buildThemeSearchPaths(parent, baseDirs, visited, searchDirs);
      }
    }
  }

  IconThemePlan buildThemePlan() {
    IconThemePlan plan;
    const auto dataDirs = xdgDataDirs();
    plan.baseDirs = iconBaseDirs(dataDirs);
    plan.pixmapDirs = pixmapDirs(dataDirs);

    std::set<std::string> visited;

    // Use the first candidate theme that actually exists on disk
    for (const auto& candidate : readGtkThemeCandidates()) {
      bool exists = false;
      for (const auto& base : plan.baseDirs) {
        if (fs::is_directory(base + "/" + candidate)) {
          exists = true;
          break;
        }
      }
      if (exists) {
        plan.activeTheme = candidate;
        buildThemeSearchPaths(candidate, plan.baseDirs, visited, plan.searchDirs);
        break;
      }
    }

    // hicolor is the mandatory base theme — always include it last
    buildThemeSearchPaths("hicolor", plan.baseDirs, visited, plan.searchDirs);
    plan.signature = signatureFor(plan);
    return plan;
  }

  void ensureThemeState() {
    auto& state = iconThemeState();
    if (!state.initialized) {
      state.plan = buildThemePlan();
      state.initialized = true;
    }
  }

} // namespace

IconResolver::IconResolver() { rebuild(); }

bool IconResolver::checkThemeChanged() {
  auto& state = iconThemeState();
  IconThemePlan next = buildThemePlan();
  if (!state.initialized) {
    state.plan = std::move(next);
    state.initialized = true;
    return false;
  }
  if (next.signature == state.plan.signature) {
    return false;
  }

  state.plan = std::move(next);
  ++state.generation;
  return true;
}

std::uint64_t IconResolver::themeGeneration() {
  ensureThemeState();
  return iconThemeState().generation;
}

void IconResolver::rebuild() {
  ensureThemeState();
  const auto& state = iconThemeState();
  m_baseDirs = state.plan.baseDirs;
  m_searchDirs = state.plan.searchDirs;
  m_pixmapDirs = state.plan.pixmapDirs;
  m_cache.clear();
  m_generation = state.generation;
}

void IconResolver::ensureFresh() {
  if (m_generation != themeGeneration()) {
    rebuild();
  }
}

const std::string& IconResolver::resolve(const std::string& iconName, int targetSize) {
  if (iconName.empty()) {
    return m_empty;
  }
  ensureFresh();
  const std::string key = iconName + '\x1f' + std::to_string(std::max(0, targetSize));
  auto it = m_cache.find(key);
  if (it != m_cache.end()) {
    return it->second;
  }
  auto icon = findIcon(iconName, targetSize);
  if (icon.empty()) {
    return m_empty;
  }
  auto [ins, _] = m_cache.emplace(key, icon);
  return ins->second;
}

std::string IconResolver::findIcon(const std::string& name, int targetSize) const {
  // Absolute path — use directly
  if (!name.empty() && name[0] == '/') {
    return fs::exists(name) ? name : std::string{};
  }

  if (targetSize <= 0) {
    // Legacy behavior: search dirs are pre-sorted scalable-first then largest;
    // return the first match. Callers with no size budget keep this exactly.
    for (const auto& dir : m_searchDirs) {
      for (const char* ext : {".svg", ".png"}) {
        std::string path = dir.path + name + ext;
        if (fs::exists(path)) {
          return path;
        }
      }
    }
  } else {
    // Size-aware: a vector icon is crisp at any size, so an SVG always wins
    // (first match honours theme inheritance order).
    for (const auto& dir : m_searchDirs) {
      std::string svg = dir.path + name + ".svg";
      if (fs::exists(svg)) {
        return svg;
      }
    }

    // Among bitmaps, prefer the smallest theme size that is still >= the
    // requested size (gentle downscale); otherwise the largest available
    // (least upscaling). Unknown-size dirs are a last resort.
    std::string best;
    int bestSize = 0;
    bool bestIsUpscale = true;
    for (const auto& dir : m_searchDirs) {
      std::string png = dir.path + name + ".png";
      if (!fs::exists(png)) {
        continue;
      }
      const int size = dir.size;
      const bool isUpscale = size < targetSize; // size 0 (unknown) counts as upscale
      bool better = false;
      if (best.empty()) {
        better = true;
      } else if (bestIsUpscale != isUpscale) {
        better = !isUpscale; // a downscale source always beats an upscale one
      } else if (isUpscale) {
        better = size > bestSize; // upscaling: the bigger the source the better
      } else {
        better = size < bestSize; // downscaling: the closer to target the better
      }
      if (better) {
        best = std::move(png);
        bestSize = size;
        bestIsUpscale = isUpscale;
      }
    }
    if (!best.empty()) {
      return best;
    }
  }

  // Fallback: pixmaps
  for (const auto& dir : m_pixmapDirs) {
    for (const char* ext : {".svg", ".png"}) {
      std::string path = dir + "/" + name + ext;
      if (fs::exists(path)) {
        return path;
      }
    }
  }

  return {};
}
