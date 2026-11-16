#include "system/icon_resolver.h"

#include "core/log.h"
#include "system/freedesktop_key_file.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cstdlib>
#include <dirent.h>
#include <filesystem>
#include <format>
#include <gio/gio.h>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace fs = std::filesystem;

class IconThemeCache;

// What one search directory contains, read once and shared by all resolvers.
struct IconDirIndex {
  std::shared_ptr<const IconThemeCache> cache;
  std::uint16_t cacheDirectory = 0;
  // Sorted name hashes, used when GTK's cache doesn't cover the directory.
  std::vector<std::size_t> entryHashes;
};

// GTK's cache for a theme root, if it has a valid one, and the names in the root itself.
struct IconThemeRoot {
  std::shared_ptr<const IconThemeCache> cache;
  // Sorted name hashes.
  std::vector<std::size_t> entryHashes;
};

namespace {

  constexpr Logger kLog("icon-resolver");
  constexpr std::string_view kSvgExtension = ".svg";
  constexpr std::string_view kPngExtension = ".png";
  // Image flags in GTK's icon-theme.cache.
  constexpr std::uint16_t kCacheSvgFlag = 1 << 1;
  constexpr std::uint16_t kCachePngFlag = 1 << 2;

  struct IconThemePlan {
    std::vector<std::string> baseDirs;
    std::vector<std::string> themeRoots;
    std::vector<IconSearchDir> searchDirs;
    std::vector<std::string> pixmapDirs;
    std::string activeTheme;
    std::string planSignature;
    std::string contentSignature;
  };

  void pushUniqueDir(std::vector<IconSearchDir>& dirs, IconSearchDir dir) {
    if (dir.path.empty()) {
      return;
    }
    if (!std::ranges::contains(dirs, dir.path, &IconSearchDir::path)) {
      dirs.push_back(std::move(dir));
    }
  }

  // Nominal size encoded in a well-known subdir name like "48x48" or "scalable".
  int sizeFromDirName(std::string_view dirName) {
    if (dirName.contains("scalable")) {
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

  // Shared across all IconResolver instances, including thread_local ones on
  // script worker threads; every access goes through `mutex`.
  struct IconThemeState {
    std::mutex mutex;
    bool initialized = false;
    std::uint64_t generation = 1;
    IconThemePlan plan;
    // Parallel to plan.searchDirs; each index is built on first use.
    std::vector<std::shared_ptr<const IconDirIndex>> dirIndexes;
    std::unordered_map<std::string, std::shared_ptr<const IconThemeRoot>> themeRoots;
    // Bumped when the indexes are dropped, so one that was being built at the time isn't kept.
    std::uint64_t indexEpoch = 0;
  };

  // Requires state.mutex to be held.
  void startGenerationLocked(IconThemeState& state) {
    ++state.generation;
    state.dirIndexes.clear();
    state.themeRoots.clear();
  }

  IconThemeState& iconThemeState() {
    static IconThemeState state;
    return state;
  }

  std::string trimAndUnquote(std::string_view value) { return StringUtils::unquote(StringUtils::trim(value)); }

  void pushUnique(std::vector<std::string>& values, std::string value) {
    if (value.empty()) {
      return;
    }
    if (!std::ranges::contains(values, value)) {
      values.push_back(std::move(value));
    }
  }

  bool pathExists(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path, ec);
  }

  bool isSymbolicIconName(std::string_view name) {
    if (const auto slash = name.find_last_of('/'); slash != std::string_view::npos) {
      name.remove_prefix(slash + 1);
    }
    return name.ends_with("-symbolic")
        || name.ends_with("-symbolic.svg")
        || name.ends_with("-symbolic.SVG")
        || name.ends_with("-symbolic.png")
        || name.ends_with("-symbolic.PNG");
  }

  bool pathIsDirectory(const fs::path& path) {
    std::error_code ec;
    const bool isDirectory = fs::is_directory(path, ec);
    if (ec == std::errc::permission_denied || ec == std::errc::operation_not_permitted) {
      static std::mutex logMutex;
      static std::unordered_set<std::string> loggedPaths;
      std::scoped_lock lock(logMutex);
      const std::string pathString = path.string();
      if (loggedPaths.emplace(pathString).second) {
        kLog.warn("cannot access icon directory '{}': {}", pathString, ec.message());
      }
    }
    return isDirectory;
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

  void appendPathIdentity(std::string& signature, std::string_view kind, const std::string& path) {
    signature += kind;
    signature += path;
    signature += ':';
    std::error_code ec;
    const auto canonical = fs::weakly_canonical(path, ec);
    signature += ec ? "missing" : canonical.string();
    signature += '\n';
  }

  std::string planSignatureFor(const IconThemePlan& plan) {
    std::string signature = plan.activeTheme;
    signature += '\n';
    for (const auto& root : plan.baseDirs) {
      appendPathIdentity(signature, "base:", root);
    }
    for (const auto& root : plan.themeRoots) {
      appendPathIdentity(signature, "theme:", root);
    }
    for (const auto& dir : plan.searchDirs) {
      signature += "search:";
      signature += dir.path;
      signature += ':';
      signature += std::to_string(dir.size);
      signature += dir.scalable ? ":scalable:" : ":fixed:";
      signature += dir.theme;
      signature += ':';
      signature += std::to_string(dir.minSize);
      signature += ':';
      signature += std::to_string(dir.maxSize);
      signature += '\n';
    }
    for (const auto& dir : plan.pixmapDirs) {
      appendPathIdentity(signature, "pixmap:", dir);
    }
    return signature;
  }

  std::string contentSignatureFor(const IconThemePlan& plan) {
    std::string signature;
    const auto appendDirectory = [&](const std::string& path) {
      signature += path;
      signature += ':';
      std::error_code ec;
      const auto modified = fs::last_write_time(path, ec);
      if (ec) {
        signature += "missing";
      } else {
        signature += std::to_string(static_cast<std::int64_t>(modified.time_since_epoch().count()));
      }
      signature += '\n';
    };
    for (const auto& dir : plan.searchDirs) {
      appendDirectory(dir.path);
    }
    for (const auto& dir : plan.baseDirs) {
      appendDirectory(dir);
    }
    for (const auto& dir : plan.pixmapDirs) {
      appendDirectory(dir);
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
    static std::unique_ptr<GSettings, GSettingsDeleter> settings = [] {
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
        const fs::path path = std::string(home) + cfg;
        const auto parsed = freedesktop::parseKeyFile(path);
        if (!parsed) {
          if (parsed.error().reason != freedesktop::ParseFailure::NotFound) {
            kLog.warn("failed to parse GTK settings '{}': {}", path.string(), parsed.error().message);
          }
          continue;
        }
        for (const auto& diagnostic : parsed->diagnostics) {
          kLog.debug(
              "ignoring malformed GTK settings entry '{}:{}: {}'", path.string(), diagnostic.line, diagnostic.message
          );
        }
        if (const auto value = parsed->file.value("Settings", "gtk-icon-theme-name"); value.has_value()) {
          const std::string theme = trimAndUnquote(*value);
          if (!theme.empty()) {
            candidates.emplace_back(theme);
          }
        }
      }
    }

    return candidates;
  }

  // Parse index.theme and return (subdir paths sorted by preference, parent theme names).
  // Preference: scalable/large dirs first so we get crisp icons at any size.
  std::pair<std::vector<IconSearchDir>, std::vector<std::string>> parseIndexTheme(const std::string& themeRoot) {
    const fs::path indexPath = fs::path(themeRoot) / "index.theme";
    const auto parsed = freedesktop::parseKeyFile(indexPath);
    if (!parsed) {
      if (parsed.error().reason != freedesktop::ParseFailure::NotFound) {
        kLog.warn("failed to parse icon theme metadata '{}': {}", indexPath.string(), parsed.error().message);
      }
      return {};
    }
    for (const auto& diagnostic : parsed->diagnostics) {
      kLog.debug(
          "ignoring malformed index.theme entry '{}:{}: {}'", indexPath.string(), diagnostic.line, diagnostic.message
      );
    }

    struct DirEntry {
      std::string path;
      int size = 0;
      int maxSize = 0;
      int minSize = 0;
      int threshold = 2;
      std::string type = "Threshold";
      bool scalable = false;
    };

    std::vector<std::string> dirNames;
    std::vector<std::string> inherits;
    std::unordered_map<std::string, DirEntry> dirMap;

    if (const auto directories = parsed->file.value("Icon Theme", "Directories"); directories.has_value()) {
      for (auto name : splitList(*directories, ',')) {
        dirNames.emplace_back(name);
        dirMap[std::move(name)].path = dirNames.back();
      }
    }
    if (const auto inherited = parsed->file.value("Icon Theme", "Inherits"); inherited.has_value()) {
      for (auto name : splitList(*inherited, ',')) {
        inherits.emplace_back(std::move(name));
      }
    }

    for (const auto& name : dirNames) {
      const auto* section = parsed->file.section(name);
      if (section == nullptr) {
        continue;
      }
      auto& entry = dirMap[name];
      if (const auto it = section->find("Size"); it != section->end()) {
        try {
          entry.size = std::stoi(it->second);
        } catch (...) {
        }
      }
      if (const auto it = section->find("Type"); it != section->end()) {
        entry.type = it->second;
        entry.scalable = (entry.type == "Scalable" || entry.type == "Threshold");
      }
      if (const auto it = section->find("MaxSize"); it != section->end()) {
        try {
          entry.maxSize = std::stoi(it->second);
        } catch (...) {
        }
      }
      entry.minSize = entry.size;
      if (const auto it = section->find("MinSize"); it != section->end()) {
        try {
          entry.minSize = std::stoi(it->second);
        } catch (...) {
        }
      }
      if (const auto it = section->find("Threshold"); it != section->end()) {
        try {
          entry.threshold = std::max(0, std::stoi(it->second));
        } catch (...) {
        }
      }
    }

    // Sort dirs: scalable first, then by size descending (MaxSize first, then use Size as a tiebreaker)
    std::ranges::stable_sort(dirNames, [&](const std::string& a, const std::string& b) {
      const auto& da = dirMap[a];
      const auto& db = dirMap[b];
      if (da.scalable != db.scalable) {
        return da.scalable > db.scalable;
      }
      if (da.maxSize != db.maxSize) {
        return da.maxSize > db.maxSize;
      }
      return da.size > db.size;
    });

    std::vector<IconSearchDir> sortedPaths;
    sortedPaths.reserve(dirNames.size());
    for (const auto& name : dirNames) {
      const auto& entry = dirMap[name];
      int minSize = entry.size;
      int maxSize = entry.size;
      if (entry.type == "Threshold") {
        minSize = std::max(1, entry.size - entry.threshold);
        maxSize = entry.size + entry.threshold;
      } else if (entry.type == "Scalable") {
        minSize = entry.minSize;
        maxSize = entry.maxSize > 0 ? entry.maxSize : entry.size;
      }
      sortedPaths.push_back(
          IconSearchDir{
              .path = name,
              .size = entry.size,
              .scalable = entry.scalable,
              .theme = {},
              .minSize = minSize,
              .maxSize = std::max(minSize, maxSize)
          }
      );
    }

    return {sortedPaths, inherits};
  }

  void buildThemeSearchPaths(
      const std::string& themeName, const std::vector<std::string>& baseDirs, std::set<std::string>& visited,
      std::vector<std::string>& themeRoots, std::vector<IconSearchDir>& searchDirs
  ) {
    if (visited.contains(themeName)) {
      return;
    }
    visited.insert(themeName);

    std::vector<std::string> roots;
    std::vector<IconSearchDir> dirs;
    std::vector<std::string> parents;
    for (const auto& base : baseDirs) {
      const std::string themeRoot = std::format("{}/{}", base, themeName);
      if (!pathIsDirectory(themeRoot)) {
        continue;
      }
      pushUnique(themeRoots, themeRoot);
      roots.push_back(themeRoot);
      if (dirs.empty()) {
        auto [parsedDirs, inherits] = parseIndexTheme(themeRoot);
        if (!parsedDirs.empty()) {
          dirs = std::move(parsedDirs);
          parents = std::move(inherits);
        } else {
          for (const auto& parent : inherits) {
            pushUnique(parents, parent);
          }
        }
      }
    }

    // The first index.theme describes every root of this logical theme.
    for (const auto& themeRoot : roots) {
      if (dirs.empty()) {
        // No index.theme — fall back to common paths so the theme isn't silently skipped
        for (const char* path :
             {"/scalable/apps/", "/512x512/apps/", "/256x256/apps/", "/128x128/apps/", "/64x64/apps/", "/48x48/apps/",
              "/32x32/apps/", "/"}) {
          const std::string_view name(path);
          pushUniqueDir(
              searchDirs,
              IconSearchDir{
                  .path = themeRoot + path,
                  .size = sizeFromDirName(name),
                  .scalable = name.contains("scalable"),
                  .theme = themeName,
                  .minSize = sizeFromDirName(name),
                  .maxSize = sizeFromDirName(name)
              }
          );
        }
      } else {
        for (const auto& dir : dirs) {
          pushUniqueDir(
              searchDirs,
              IconSearchDir{
                  .path = themeRoot + "/" + dir.path + "/",
                  .size = dir.size,
                  .scalable = dir.scalable,
                  .theme = themeName,
                  .minSize = dir.minSize,
                  .maxSize = dir.maxSize
              }
          );
        }
      }
    }
    for (const auto& parent : parents) {
      buildThemeSearchPaths(parent, baseDirs, visited, themeRoots, searchDirs);
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
        if (pathIsDirectory(std::format("{}/{}", base, candidate))) {
          exists = true;
          break;
        }
      }
      if (exists) {
        plan.activeTheme = candidate;
        buildThemeSearchPaths(candidate, plan.baseDirs, visited, plan.themeRoots, plan.searchDirs);
        break;
      }
    }

    // hicolor is the mandatory base theme — always include it last
    buildThemeSearchPaths("hicolor", plan.baseDirs, visited, plan.themeRoots, plan.searchDirs);
    plan.planSignature = planSignatureFor(plan);
    plan.contentSignature = contentSignatureFor(plan);
    return plan;
  }

  // Requires state.mutex to be held.
  void ensureThemeStateLocked(IconThemeState& state) {
    if (!state.initialized) {
      state.plan = buildThemePlan();
      state.initialized = true;
    }
  }

  bool updateThemeStateLocked(IconThemeState& state, bool includeContentChanges) {
    IconThemePlan next = buildThemePlan();
    if (!state.initialized) {
      state.plan = std::move(next);
      state.initialized = true;
      return false;
    }
    const bool changed = next.planSignature != state.plan.planSignature
        || (includeContentChanges && next.contentSignature != state.plan.contentSignature);
    state.plan = std::move(next);
    if (changed) {
      startGenerationLocked(state);
    }
    return changed;
  }

} // namespace

IconResolver::IconResolver(bool cacheMissing) : m_cacheMissing(cacheMissing) { rebuild(); }

IconResolver::IconResolver() : IconResolver(false) {}

bool IconResolver::checkThemeChanged() {
  auto& state = iconThemeState();
  std::scoped_lock lock(state.mutex);
  return updateThemeStateLocked(state, true);
}

bool IconResolver::checkThemePlanChanged() {
  auto& state = iconThemeState();
  std::scoped_lock lock(state.mutex);
  return updateThemeStateLocked(state, false);
}

void IconResolver::invalidateThemeContent() {
  auto& state = iconThemeState();
  std::scoped_lock lock(state.mutex);
  IconThemePlan next = buildThemePlan();
  state.plan = std::move(next);
  state.initialized = true;
  startGenerationLocked(state);
}

IconThemeWatchPaths IconResolver::themeWatchPaths() {
  auto& state = iconThemeState();
  std::scoped_lock lock(state.mutex);
  ensureThemeStateLocked(state);
  IconThemeWatchPaths paths{
      .baseDirs = state.plan.baseDirs,
      .themeRoots = state.plan.themeRoots,
      .iconDirs = state.plan.pixmapDirs,
      .settingsFiles = {},
  };
  paths.iconDirs.reserve(paths.iconDirs.size() + state.plan.searchDirs.size());
  for (const auto& dir : state.plan.searchDirs) {
    pushUnique(paths.iconDirs, dir.path);
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    paths.settingsFiles.emplace_back(std::string(home) + "/.config/gtk-3.0/settings.ini");
    paths.settingsFiles.emplace_back(std::string(home) + "/.config/gtk-4.0/settings.ini");
  }
  return paths;
}

std::uint64_t IconResolver::themeGeneration() {
  auto& state = iconThemeState();
  std::scoped_lock lock(state.mutex);
  ensureThemeStateLocked(state);
  return state.generation;
}

std::string IconResolver::activeThemeName() {
  auto& state = iconThemeState();
  std::scoped_lock lock(state.mutex);
  ensureThemeStateLocked(state);
  return state.plan.activeTheme;
}

void IconResolver::rebuild() {
  auto& state = iconThemeState();
  std::scoped_lock lock(state.mutex);
  ensureThemeStateLocked(state);
  m_baseDirs = state.plan.baseDirs;
  m_searchDirs = state.plan.searchDirs;
  m_pixmapDirs = state.plan.pixmapDirs;
  m_cache.clear();
  m_symbolicPreferredCache.clear();
  m_missingCache.clear();
  m_dirIndexes.assign(m_searchDirs.size(), nullptr);
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
  const std::string key = iconName + '\x1F' + std::to_string(std::max(0, targetSize));
  auto it = m_cache.find(key);
  if (it != m_cache.end()) {
    if (iconName.front() != '/' || pathExists(it->second)) {
      return it->second;
    }
    m_cache.erase(it);
  }
  const bool canCacheMissing = m_cacheMissing && iconName.front() != '/';
  if (canCacheMissing && m_missingCache.contains(key)) {
    return m_empty;
  }
  auto icon = findIcon(iconName, targetSize);
  if (icon.empty()) {
    if (canCacheMissing) {
      m_missingCache.emplace(key);
    }
    return m_empty;
  }
  auto [ins, _] = m_cache.emplace(key, icon);
  return ins->second;
}

ResolvedIcon IconResolver::resolveSymbolicPreferred(const std::string& iconName, int targetSize) {
  if (iconName.empty()) {
    return {};
  }

  ensureFresh();
  const int normalizedSize = std::max(0, targetSize);
  const bool cacheable = iconName.front() != '/';
  const std::string key = iconName + '\x1F' + std::to_string(normalizedSize);
  if (cacheable) {
    if (const auto it = m_symbolicPreferredCache.find(key); it != m_symbolicPreferredCache.end()) {
      return it->second;
    }
  }

  ResolvedIcon result;
  const bool explicitSymbolic = isSymbolicIconName(iconName);
  if (!explicitSymbolic && cacheable) {
    const std::string symbolicName = iconName + "-symbolic";
    if (const std::string& symbolic = resolve(symbolicName, normalizedSize); !symbolic.empty()) {
      result = ResolvedIcon{.path = symbolic, .symbolic = true};
    }
  }

  if (result.path.empty()) {
    if (const std::string& resolved = resolve(iconName, normalizedSize); !resolved.empty()) {
      result = ResolvedIcon{.path = resolved, .symbolic = explicitSymbolic};
    } else if (explicitSymbolic && cacheable && iconName.ends_with("-symbolic")) {
      const std::string regularName = iconName.substr(0, iconName.size() - std::string_view("-symbolic").size());
      if (const std::string& regular = resolve(regularName, normalizedSize); !regular.empty()) {
        result = ResolvedIcon{.path = regular, .symbolic = false};
      }
    }
  }

  if (cacheable && !result.path.empty()) {
    m_symbolicPreferredCache.insert_or_assign(key, result);
  }
  return result;
}

std::string IconResolver::resolveStatusVector(const std::string& iconName, int targetSize) {
  if (iconName.empty()) {
    return {};
  }
  std::string normal = resolve(iconName, std::max(1, targetSize));
  const auto isStatusVector = [](std::string_view path) {
    return path.ends_with(".svg") && (path.contains("/status/") || path.contains("/panel/"));
  };
  if (!isStatusVector(normal)) {
    return {};
  }
  if (iconName.front() == '/') {
    return normal;
  }

  const auto owner = std::ranges::find_if(m_searchDirs, [&](const IconSearchDir& dir) {
    return dir.path + iconName + ".svg" == normal;
  });
  if (owner == m_searchDirs.end()) {
    return normal;
  }

  targetSize = std::max(1, targetSize);
  std::string best = normal;
  int bestDistance = std::numeric_limits<int>::max();
  int bestNominalDistance = std::numeric_limits<int>::max();
  for (const auto& dir : m_searchDirs) {
    if (dir.theme != owner->theme) {
      continue;
    }
    const std::string path = dir.path + iconName + ".svg";
    if (!isStatusVector(path) || !pathExists(path)) {
      continue;
    }

    // Prefer the allowed range, then its nominal artwork size. Equal candidates
    // retain the existing directory and base-root order.
    const int distance = dir.size > 0 ? std::max({0, dir.minSize - targetSize, targetSize - dir.maxSize})
                                      : std::numeric_limits<int>::max();
    const int nominalDistance = dir.size > 0 ? std::abs(dir.size - targetSize) : std::numeric_limits<int>::max();
    if (distance < bestDistance || (distance == bestDistance && nominalDistance < bestNominalDistance)) {
      best = path;
      bestDistance = distance;
      bestNominalDistance = nominalDistance;
    }
  }
  return best;
}

void IconResolver::invalidateMissingCache() {
  m_missingCache.clear();
  m_symbolicPreferredCache.clear();
  m_dirIndexes.assign(m_searchDirs.size(), nullptr);
  // Drop the shared indexes too, so new lookups see the current directory contents.
  auto& state = iconThemeState();
  std::scoped_lock lock(state.mutex);
  if (state.generation == m_generation) {
    state.dirIndexes.clear();
    state.themeRoots.clear();
    ++state.indexEpoch;
  }
}

// Reader for the icon-theme.cache written by gtk-update-icon-cache (big-endian records linked by offsets).
class IconThemeCache {
public:
  using MappedFile = std::unique_ptr<GMappedFile, decltype(&g_mapped_file_unref)>;

  IconThemeCache(MappedFile file, fs::file_time_type mtime)
      : m_file(std::move(file)),
        m_data(g_mapped_file_get_contents(m_file.get()), g_mapped_file_get_length(m_file.get())), m_mtime(mtime) {}

  static std::shared_ptr<const IconThemeCache> load(const std::string& themeRoot) {
    std::error_code ec;
    const fs::path path = fs::canonical(std::format("{}/{}", themeRoot, kFileName), ec);
    if (ec) {
      return nullptr;
    }
    // In a theme merged from several packages, the cache may be a symlink to one package's cache,
    // which only lists that package's icons.
    const fs::path root = fs::canonical(themeRoot, ec);
    if (ec || path.parent_path() != root) {
      return nullptr;
    }
    const auto mtime = fs::last_write_time(path, ec);
    if (ec) {
      return nullptr;
    }
    GError* error = nullptr;
    MappedFile file(g_mapped_file_new(path.c_str(), FALSE, &error), &g_mapped_file_unref);
    if (file == nullptr) {
      kLog.warn("failed to map icon cache '{}': {}", path.string(), error->message);
      g_error_free(error);
      return nullptr;
    }
    auto cache = std::make_shared<IconThemeCache>(std::move(file), mtime);
    // Unlisted directories are treated as empty, so the directory list has to be readable in full.
    if (cache->read16(kMajorVersionField) != kMajorVersion
        || cache->read16(kMinorVersionField) != kMinorVersion
        || !cache->readDirectories()) {
      kLog.warn("invalid icon cache '{}'", path.string());
      return nullptr;
    }
    return cache;
  }

  fs::file_time_type mtime() const noexcept { return m_mtime; }

  std::optional<std::uint16_t> directoryIndex(std::string_view subdir) const {
    const auto it = std::ranges::find(m_directories, subdir);
    if (it == m_directories.end()) {
      return std::nullopt;
    }
    return static_cast<std::uint16_t>(it - m_directories.begin());
  }

  bool hasImage(std::string_view iconName, std::uint16_t directory, std::uint16_t formatFlag) const {
    const auto table = read32(kHashTableField);
    const auto buckets = table ? read32(*table) : std::nullopt;
    if (!buckets || *buckets == 0) {
      return false;
    }
    auto entry = read32(*table + kOffsetSize * (1 + nameHash(iconName) % *buckets));
    // Every entry takes several bytes, so more steps than bytes means the chain loops.
    for (std::size_t steps = 0; entry && *entry != kNoEntry && steps < m_data.size(); ++steps) {
      const auto nameOffset = read32(*entry + kEntryNameField);
      const auto name = nameOffset ? stringAt(*nameOffset) : std::nullopt;
      if (name == iconName) {
        return (imageFlags(*entry, directory).value_or(0) & formatFlag) != 0;
      }
      entry = read32(*entry + kEntryChainField);
    }
    return false;
  }

private:
  static constexpr std::string_view kFileName = "icon-theme.cache";
  static constexpr std::uint16_t kMajorVersion = 1;
  static constexpr std::uint16_t kMinorVersion = 0;
  static constexpr std::uint32_t kNoEntry = 0xFFFFFFFF;
  static constexpr std::size_t kOffsetSize = sizeof(std::uint32_t);
  static constexpr std::size_t kMajorVersionField = 0;
  static constexpr std::size_t kMinorVersionField = 2;
  static constexpr std::size_t kHashTableField = 4;
  static constexpr std::size_t kDirectoryListField = 8;
  static constexpr std::size_t kEntryChainField = 0;
  static constexpr std::size_t kEntryNameField = 4;
  static constexpr std::size_t kEntryImageListField = 8;
  static constexpr std::size_t kImageSize = 8;
  static constexpr std::size_t kImageFlagsField = 2;

  // Matches GTK's icon_name_hash(), which hashes the name as signed chars.
  static std::uint32_t nameHash(std::string_view name) {
    std::uint32_t hash = 0;
    for (const char c : name) {
      hash = (hash << 5) - hash + static_cast<std::uint32_t>(static_cast<signed char>(c));
    }
    return hash;
  }

  bool readDirectories() {
    const auto list = read32(kDirectoryListField);
    const auto count = list ? read32(*list) : std::nullopt;
    // Images reference directories by a 16-bit index.
    if (!count || *count > std::numeric_limits<std::uint16_t>::max() + std::size_t{1}) {
      return false;
    }
    for (std::size_t i = 0; i < *count; ++i) {
      const auto nameOffset = read32(*list + kOffsetSize * (i + 1));
      const auto name = nameOffset ? stringAt(*nameOffset) : std::nullopt;
      if (!name) {
        return false;
      }
      m_directories.push_back(*name);
    }
    return true;
  }

  std::optional<std::uint16_t> imageFlags(std::size_t entry, std::uint16_t directory) const {
    const auto images = read32(entry + kEntryImageListField);
    const auto count = images ? read32(*images) : std::nullopt;
    if (!count) {
      return std::nullopt;
    }
    for (std::size_t i = 0; i < *count; ++i) {
      const std::size_t image = *images + kOffsetSize + kImageSize * i;
      const auto imageDirectory = read16(image);
      if (!imageDirectory) {
        return std::nullopt;
      }
      if (*imageDirectory == directory) {
        return read16(image + kImageFlagsField);
      }
    }
    return std::nullopt;
  }

  std::optional<std::uint16_t> read16(std::size_t offset) const {
    if (offset > m_data.size() || m_data.size() - offset < sizeof(std::uint16_t)) {
      return std::nullopt;
    }
    return static_cast<std::uint16_t>(byteAt(offset) << 8 | byteAt(offset + 1));
  }

  std::optional<std::uint32_t> read32(std::size_t offset) const {
    if (offset > m_data.size() || m_data.size() - offset < sizeof(std::uint32_t)) {
      return std::nullopt;
    }
    return byteAt(offset) << 24 | byteAt(offset + 1) << 16 | byteAt(offset + 2) << 8 | byteAt(offset + 3);
  }

  std::uint32_t byteAt(std::size_t offset) const { return static_cast<unsigned char>(m_data[offset]); }

  std::optional<std::string_view> stringAt(std::size_t offset) const {
    const auto end = offset < m_data.size() ? m_data.find('\0', offset) : std::string_view::npos;
    if (end == std::string_view::npos) {
      return std::nullopt;
    }
    return m_data.substr(offset, end - offset);
  }

  MappedFile m_file;
  std::string_view m_data;
  fs::file_time_type m_mtime;
  std::vector<std::string_view> m_directories;
};

namespace {

  // Storing hashes keeps Papirus (about 300k files) to a few MB.
  std::vector<std::size_t> listEntryHashes(const std::string& path) {
    std::vector<std::size_t> hashes;
    if (DIR* handle = ::opendir(path.c_str()); handle != nullptr) {
      while (const dirent* entry = ::readdir(handle)) {
        hashes.push_back(std::hash<std::string_view>{}(entry->d_name));
      }
      ::closedir(handle);
      std::ranges::sort(hashes);
    }
    return hashes;
  }

  std::shared_ptr<const IconThemeRoot>
  sharedThemeRoot(IconThemeState& state, std::uint64_t generation, const std::string& themeRoot) {
    std::uint64_t epoch = 0;
    {
      std::scoped_lock lock(state.mutex);
      if (const auto it = state.themeRoots.find(themeRoot); it != state.themeRoots.end()) {
        return it->second;
      }
      epoch = state.indexEpoch;
    }
    auto root = std::make_shared<const IconThemeRoot>(IconThemeCache::load(themeRoot), listEntryHashes(themeRoot));
    std::scoped_lock lock(state.mutex);
    if (state.generation != generation || state.indexEpoch != epoch) {
      return root;
    }
    return state.themeRoots.try_emplace(themeRoot, std::move(root)).first->second;
  }

  std::shared_ptr<const IconDirIndex> buildDirIndex(
      IconThemeState& state, std::uint64_t generation, const IconSearchDir& dir,
      const std::vector<std::string>& baseDirs
  ) {
    auto index = std::make_shared<IconDirIndex>();
    const auto base = std::ranges::find_if(baseDirs, [&dir](const std::string& baseDir) {
      return dir.path.starts_with(std::format("{}/{}/", baseDir, dir.theme));
    });
    if (base != baseDirs.end()) {
      const std::string themeRoot = std::format("{}/{}", *base, dir.theme);
      const auto root = sharedThemeRoot(state, generation, themeRoot);
      std::string_view subdir = std::string_view(dir.path).substr(themeRoot.size() + 1);
      if (subdir.ends_with('/')) {
        subdir.remove_suffix(1);
      }
      // GTK's cache never lists the theme root, which is searched when a theme has no index.theme.
      if (subdir.empty()) {
        index->entryHashes = root->entryHashes;
        return index;
      }
      // Skip directories the root doesn't contain without an opendir; most data dirs have only a
      // few of the hundreds a theme lists.
      const std::string_view topLevel = subdir.substr(0, subdir.find('/'));
      if (!topLevel.empty()
          && !std::ranges::binary_search(root->entryHashes, std::hash<std::string_view>{}(topLevel))) {
        return index;
      }
      if (root->cache != nullptr) {
        std::error_code ec;
        const auto dirTime = fs::last_write_time(dir.path, ec);
        if (ec) {
          return index;
        }
        // Use the cache for directories no newer than it. A directory it doesn't list has no icons.
        if (dirTime <= root->cache->mtime()) {
          if (const auto cacheDirectory = root->cache->directoryIndex(subdir)) {
            index->cache = root->cache;
            index->cacheDirectory = *cacheDirectory;
          }
          return index;
        }
      }
    }
    index->entryHashes = listEntryHashes(dir.path);
    return index;
  }

} // namespace

const IconDirIndex& IconResolver::dirIndex(std::size_t searchDir) const {
  if (m_dirIndexes[searchDir] != nullptr) {
    return *m_dirIndexes[searchDir];
  }
  auto& state = iconThemeState();
  std::uint64_t epoch = 0;
  {
    std::scoped_lock lock(state.mutex);
    if (state.generation == m_generation
        && searchDir < state.dirIndexes.size()
        && state.dirIndexes[searchDir] != nullptr) {
      m_dirIndexes[searchDir] = state.dirIndexes[searchDir];
      return *m_dirIndexes[searchDir];
    }
    epoch = state.indexEpoch;
  }
  // Build without the lock so other threads' lookups don't wait on disk I/O.
  auto index = buildDirIndex(state, m_generation, m_searchDirs[searchDir], m_baseDirs);
  {
    std::scoped_lock lock(state.mutex);
    if (state.generation == m_generation && state.indexEpoch == epoch) {
      state.dirIndexes.resize(state.plan.searchDirs.size());
      auto& shared = state.dirIndexes[searchDir];
      if (shared == nullptr) {
        shared = index;
      } else {
        index = shared;
      }
    }
  }
  m_dirIndexes[searchDir] = std::move(index);
  return *m_dirIndexes[searchDir];
}

bool IconResolver::dirHasIcon(std::size_t searchDir, const std::string& name, std::string_view extension) const {
  const IconSearchDir& dir = m_searchDirs[searchDir];
  if (name.contains('/')) {
    return pathExists(std::format("{}{}{}", dir.path, name, extension));
  }
  const IconDirIndex& index = dirIndex(searchDir);
  bool listed = false;
  if (index.cache != nullptr) {
    const std::uint16_t flag = extension == kSvgExtension ? kCacheSvgFlag : kCachePngFlag;
    listed = index.cache->hasImage(name, index.cacheDirectory, flag);
  } else if (!index.entryHashes.empty()) {
    const std::string fileName = std::format("{}{}", name, extension);
    listed = std::ranges::binary_search(index.entryHashes, std::hash<std::string_view>{}(fileName));
  }
  // The index can be wrong (hash collision, broken symlink, outdated cache), so check the file itself.
  return listed && pathExists(std::format("{}{}{}", dir.path, name, extension));
}

std::string IconResolver::findIcon(const std::string& name, int targetSize) const {
  // Absolute path — use directly
  if (!name.empty() && name[0] == '/') {
    return pathExists(name) ? name : std::string{};
  }

  if (targetSize <= 0) {
    // Legacy behavior: search dirs are pre-sorted scalable-first then largest;
    // return the first match. Callers with no size budget keep this exactly.
    for (std::size_t i = 0; i < m_searchDirs.size(); ++i) {
      const IconSearchDir& dir = m_searchDirs[i];
      for (const std::string_view ext : {kSvgExtension, kPngExtension}) {
        if (dirHasIcon(i, name, ext)) {
          return std::format("{}{}{}", dir.path, name, ext);
        }
      }
    }
  } else {
    // Search dirs are grouped by theme; stop at the end of the first theme with
    // a match. Within it an SVG wins; among bitmaps, prefer the smallest theme
    // size that is still >= the requested size (gentle downscale); otherwise the
    // largest available (least upscaling). Unknown-size dirs are a last resort.
    std::string best;
    std::string_view theme;
    int bestSize = 0;
    bool bestIsUpscale = true;
    for (std::size_t i = 0; i < m_searchDirs.size(); ++i) {
      const IconSearchDir& dir = m_searchDirs[i];
      if (dir.theme != theme) {
        if (!best.empty()) {
          return best;
        }
        theme = dir.theme;
      }
      if (dirHasIcon(i, name, kSvgExtension)) {
        return std::format("{}{}{}", dir.path, name, kSvgExtension);
      }
      if (!dirHasIcon(i, name, kPngExtension)) {
        continue;
      }
      std::string png = std::format("{}{}{}", dir.path, name, kPngExtension);
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

  // Fallback: unthemed icons at the top of each base dir, then pixmaps (XDG
  // LookupFallbackIcon order).
  for (const auto& dir : m_baseDirs) {
    for (const char* ext : {".svg", ".png"}) {
      std::string path = std::format("{}/{}{}", dir, name, ext);
      if (pathExists(path)) {
        return path;
      }
    }
  }
  for (const auto& dir : m_pixmapDirs) {
    for (const char* ext : {".svg", ".png"}) {
      std::string path = std::format("{}/{}{}", dir, name, ext);
      if (pathExists(path)) {
        return path;
      }
    }
  }

  return {};
}
