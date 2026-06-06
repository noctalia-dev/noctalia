#pragma once

#include "config/config_types.h"
#include "config/state_store.h"
#include "core/toml.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class IpcService;
class NotificationManager;

class ConfigService {
public:
  using ReloadCallback = std::function<void()>;
  using ChangeCallback = std::function<void()>;

  // RAII scope that coalesces wallpaper changes: any setWallpaperPath() calls
  // inside the scope skip the per-call callback, and a single callback is
  // fired on scope exit if anything actually changed.
  class WallpaperBatch {
  public:
    explicit WallpaperBatch(ConfigService& config);
    ~WallpaperBatch();
    WallpaperBatch(const WallpaperBatch&) = delete;
    WallpaperBatch& operator=(const WallpaperBatch&) = delete;

  private:
    ConfigService& m_config;
  };

  ConfigService();
  ~ConfigService();

  ConfigService(const ConfigService&) = delete;
  ConfigService& operator=(const ConfigService&) = delete;

  [[nodiscard]] const Config& config() const noexcept { return m_config; }
  // Which sections changed in the reload currently being dispatched. Valid while
  // reload callbacks run; subscribers consult it to skip unaffected work.
  [[nodiscard]] const ConfigChangeSet& lastChange() const noexcept { return m_lastChange; }
  [[nodiscard]] bool matchesKeybind(KeybindAction action, std::uint32_t sym, std::uint32_t modifiers) const;
  [[nodiscard]] int watchFd() const noexcept { return m_inotifyFd; }
  [[nodiscard]] std::string buildSupportReport() const;
  [[nodiscard]] std::string buildMergedUserConfig() const;
  [[nodiscard]] std::string buildEffectiveConfig() const;
  [[nodiscard]] bool shouldRunSetupWizard() const;
  [[nodiscard]] std::optional<bool> stateBool(std::string_view owner, std::string_view key) const;
  [[nodiscard]] std::optional<std::string> stateString(std::string_view owner, std::string_view key) const;

  // The optional label is used only for opt-in reload profiling (NOCTALIA_PROFILE);
  // unlabeled subscribers are reported by registration index.
  void addReloadCallback(ReloadCallback callback, std::string_view label = {});
  void setNotificationManager(NotificationManager* manager);
  void checkReload();
  void forceReload();

  void registerIpc(IpcService& ipc);

  // Persisted wallpaper paths (written to settings.toml, app-managed).
  [[nodiscard]] std::string getWallpaperPath(const std::string& connectorName) const;
  [[nodiscard]] std::string getDefaultWallpaperPath() const;
  // Most recently applied wallpaper path (any output, or default). Used as the palette/template input
  // so colors are generated even when wallpaper management is only used on a subset of displays.
  [[nodiscard]] std::string getPaletteWallpaperPath() const;
  void setWallpaperPath(const std::optional<std::string>& connectorName, const std::string& path);
  void setWallpaperChangeCallback(ChangeCallback callback);

  [[nodiscard]] const std::vector<WallpaperFavorite>& wallpaperFavorites() const noexcept;
  [[nodiscard]] bool isWallpaperFavorite(std::string_view path) const;
  [[nodiscard]] const WallpaperFavorite* wallpaperFavorite(std::string_view path) const;
  void addWallpaperFavorite(std::string path, std::optional<WallpaperFavorite> preset = std::nullopt);
  void removeWallpaperFavorite(std::string_view path);
  void setWallpaperFavoriteThemeMode(std::string_view path, ThemeMode themeMode);
  void setWallpaperFavoritePaletteSource(std::string_view path, std::optional<PaletteSource> source);
  void setWallpaperFavoritePaletteSelection(std::string_view path, std::string_view value);
  void applyWallpaperSelection(
      const std::optional<std::string>& connectorName, const std::string& path, const WallpaperFavorite* applyTheme,
      const std::vector<std::string>& allConnectors
  );

  // Persist a theme-mode override to settings.toml and trigger the reload pipeline.
  void setThemeMode(ThemeMode mode);
  // Persist `[theme].source` and the palette field for that source, then reload.
  [[nodiscard]] bool setThemeColorScheme(PaletteSource source, std::string_view value);
  // Persist dock enabled override to settings.toml and trigger the reload pipeline.
  void setDockEnabled(bool enabled);
  // Persist desktop widget layout/editor state to settings.toml and trigger the reload pipeline.
  bool setDesktopWidgetsState(const DesktopWidgetsConfig& desktopWidgets);
  bool setLockscreenWidgetsState(const LockscreenWidgetsConfig& lockscreenWidgets);
  // Persist app-owned UI/runtime state to state.toml. This does not affect Config reloads.
  bool setStateBool(std::string_view owner, std::string_view key, bool value);
  bool setStateString(std::string_view owner, std::string_view key, std::string_view value);
  bool markSetupWizardCompleted();
  [[nodiscard]] bool hasOverride(const std::vector<std::string>& path) const;
  [[nodiscard]] bool hasEffectiveOverride(const std::vector<std::string>& path) const;
  [[nodiscard]] bool isOverrideOnlyBar(std::string_view name) const;
  [[nodiscard]] bool isOverrideOnlyCalendarAccount(std::string_view id) const;
  [[nodiscard]] bool canMoveBarOverride(std::string_view name, int direction) const;
  [[nodiscard]] bool canDeleteBarOverride(std::string_view name) const;
  [[nodiscard]] bool isOverrideOnlyMonitorOverride(std::string_view barName, std::string_view match) const;
  bool createBarOverride(std::string_view name);
  bool moveBarOverride(std::string_view name, int direction);
  bool renameBarOverride(std::string_view oldName, std::string_view newName);
  bool deleteBarOverride(std::string_view name);
  bool createMonitorOverride(std::string_view barName, std::string_view match);
  bool renameMonitorOverride(std::string_view barName, std::string_view oldMatch, std::string_view newMatch);
  bool deleteMonitorOverride(std::string_view barName, std::string_view match);
  bool deleteCalendarAccountOverride(std::string_view id);
  bool setOverride(const std::vector<std::string>& path, ConfigOverrideValue value);
  bool setOverrides(std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides);
  bool clearOverride(const std::vector<std::string>& path);
  bool renameOverrideTable(const std::vector<std::string>& oldPath, const std::vector<std::string>& newPath);

  [[nodiscard]] static BarConfig resolveForOutput(const BarConfig& base, const WaylandOutput& output);

  // Recursively overlays `overlay` onto `base` (tables merge, everything else
  // replaces). Public so `config validate` can reproduce loadAll's merge order.
  static void deepMerge(toml::table& base, const toml::table& overlay);

private:
  void loadAll();
  void parseConfigTable(const toml::table& tbl, Config& config, bool logSummary) const;
  [[nodiscard]] std::optional<Config> configForOverrides(const toml::table& overrides) const;
  [[nodiscard]] bool overridePathEffectiveInTable(
      const std::vector<std::string>& path, const toml::table& overrides, const Config* parsedWith = nullptr
  ) const;
  [[nodiscard]] std::size_t overridePreserveDepthForPath(const std::vector<std::string>& path) const;
  void setupWatch();
  void fireReloadCallbacks();
  void loadOverridesFromFile();
  void setConfigParseError(std::string parseError);
  bool writeOverridesToFile();
  void extractWallpaperFromOverrides();
  void extractWallpaperFromTable(const toml::table& table);
  void syncWallpaperFavoritesToOverridesTable();

  Config m_config;
  ConfigChangeSet m_lastChange;

  // Hand-authored config directory: all *.toml merged alphabetically.
  std::string m_configDir;

  // App-writable settings file (state dir): lives outside config dir so it
  // can still be written when the config dir is read-only (e.g. NixOS).
  std::string m_overridesPath;
  // App-owned UI/runtime state. This is not a config layer and is never
  // deep-merged into Config.
  StateStore m_stateStore;
  // Marker file (state dir): its existence means onboarding has been completed
  // or dismissed. Single canonical signal for the setup wizard.
  std::string m_setupMarkerPath;
  toml::table m_overridesTable;
  std::unordered_set<std::string> m_configFileBarNames;
  std::unordered_map<std::string, std::unordered_set<std::string>> m_configFileMonitorOverrideNames;
  std::unordered_set<std::string> m_configFileCalendarAccountNames;
  std::string m_defaultWallpaperPath;
  std::string m_lastWallpaperPath;
  std::unordered_map<std::string, std::string> m_monitorWallpaperPaths;
  std::vector<WallpaperFavorite> m_wallpaperFavorites;
  mutable std::unordered_map<std::string, bool> m_effectiveOverrideCache;

  std::string m_overridesParseError;
  std::string m_pendingError; // parse error from initial load, sent as notification once manager is wired up
  uint32_t m_configErrorNotificationId = 0; // ID of the active config-error notification, 0 if none
  NotificationManager* m_notificationManager = nullptr;

  // Single inotify fd, two watch descriptors (config dir + state dir).
  int m_inotifyFd = -1;
  int m_configWatchWd = -1;
  int m_overridesWatchWd = -1;
  struct SymlinkTargetWatch {
    std::string filename;
    bool overrides = false;
  };
  // Extra watches on symlink-target directories: wd -> list of filenames to match.
  std::unordered_map<int, std::vector<SymlinkTargetWatch>> m_symlinkDirWds;

  bool m_ownOverridesWritePending = false;
  int m_wallpaperBatchDepth = 0;
  bool m_wallpaperBatchDirty = false;

  ChangeCallback m_wallpaperChangeCallback;
  struct ReloadSubscriber {
    ReloadCallback callback;
    std::string label;
  };
  std::vector<ReloadSubscriber> m_reloadCallbacks;
};
