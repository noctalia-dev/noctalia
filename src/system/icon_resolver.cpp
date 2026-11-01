#include "system/icon_resolver.h"

#include "core/log.h"
#include "system/freedesktop_key_file.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cstdlib>
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

namespace {

  constexpr Logger kLog("icon-resolver");

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
          if (parsed.error().reason != freedesktop::ParseFailure::NOT_FOUND) {
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
      if (parsed.error().reason != freedesktop::ParseFailure::NOT_FOUND) {
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
      ++state.generation;
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
  ++state.generation;
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
}

std::string IconResolver::findIcon(const std::string& name, int targetSize) const {
  // Absolute path — use directly
  if (!name.empty() && name[0] == '/') {
    return pathExists(name) ? name : std::string{};
  }

  if (targetSize <= 0) {
    // Legacy behavior: search dirs are pre-sorted scalable-first then largest;
    // return the first match. Callers with no size budget keep this exactly.
    for (const auto& dir : m_searchDirs) {
      for (const char* ext : {".svg", ".png"}) {
        std::string path = dir.path + name + ext;
        if (pathExists(path)) {
          return path;
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
    for (const auto& dir : m_searchDirs) {
      if (dir.theme != theme) {
        if (!best.empty()) {
          return best;
        }
        theme = dir.theme;
      }
      std::string svg = dir.path + name + ".svg";
      if (pathExists(svg)) {
        return svg;
      }
      std::string png = dir.path + name + ".png";
      if (!pathExists(png)) {
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
