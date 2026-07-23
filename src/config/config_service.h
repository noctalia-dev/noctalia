#pragma once

#include "config/config_migrations.h"
#include "config/config_types.h"
#include "config/state_store.h"
#include "core/timer_manager.h"
#include "core/toml.h"

#include <cstdint>
#include <filesystem>
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
  [[nodiscard]] bool isLockScreenEnabled() const noexcept { return ::isLockScreenEnabled(m_config.lockscreen); }
  // Which sections changed in the reload currently being dispatched. Valid while
  // reload callbacks run; subscribers consult it to skip unaffected work.
  [[nodiscard]] const ConfigChangeSet& lastChange() const noexcept { return m_lastChange; }
  [[nodiscard]] const std::string& lastMutationError() const noexcept { return m_lastMutationError; }
  [[nodiscard]] bool matchesKeybind(KeybindAction action, std::uint32_t sym, std::uint32_t modifiers) const;
  [[nodiscard]] int watchFd() const noexcept { return m_inotifyFd; }
  [[nodiscard]] std::string buildSupportReport() const;
  [[nodiscard]] std::string buildMergedUserConfig() const;
  [[nodiscard]] std::string buildEffectiveConfig() const;
  [[nodiscard]] static std::string buildMergedUserConfigFromSources(
      std::string_view configDir, std::string_view settingsPath, std::string* error = nullptr
  );
  [[nodiscard]] static std::string buildEffectiveConfigFromSources(
      std::string_view configDir, std::string_view settingsPath, std::string* error = nullptr
  );
  [[nodiscard]] bool shouldRunSetupWizard() const;
  [[nodiscard]] std::optional<bool> stateBool(std::string_view owner, std::string_view key) const;
  [[nodiscard]] std::optional<std::string> stateString(std::string_view owner, std::string_view key) const;
  [[nodiscard]] const noctalia::config::LegacyConfigIssues& legacyConfigIssues() const noexcept {
    return m_legacyConfigIssues;
  }

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
  // Last applied wallpaper, else default. Drives palette generation and template previews.
  [[nodiscard]] std::string getPaletteWallpaperPath() const;
  // Greeter sync fallback when no monitor-specific path is chosen.
  [[nodiscard]] std::string getGreeterSyncWallpaperPath() const;
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

  // Add/remove a plugin id ("author/plugin") from the effective [plugins].enabled
  // list. Persists the resulting override list to settings.toml and triggers the
  // reload pipeline. No-op if already in that state.
  void setPluginEnabled(std::string_view pluginId, bool enabled);

  // Add (replacing any same-named entry) or remove a plugin source in
  // [[plugins.source]], then trigger the reload pipeline.
  void addPluginSource(const PluginSourceConfig& source);
  void removePluginSource(std::string_view name);

  // Persist the global [plugins].auto_update override to settings.toml and trigger the
  // reload pipeline. Drives background auto-update of every git source.
  void setPluginsAutoUpdate(bool enabled);

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
  bool clearStateOwner(std::string_view owner);
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
  bool setOverride(const std::vector<std::string>& path, ConfigOverrideValue value, bool* changed);
  [[nodiscard]] bool validateOverride(
      const std::vector<std::string>& path, const ConfigOverrideValue& value, std::string* error = nullptr
  );
  bool setOverrides(std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides);
  bool setOverrides(std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides, bool* changed);
  bool clearOverride(const std::vector<std::string>& path);
  bool clearOverrides(const std::vector<std::vector<std::string>>& paths, bool* changed);
  bool renameOverrideTable(const std::vector<std::string>& oldPath, const std::vector<std::string>& newPath);

  [[nodiscard]] static BarConfig resolveForOutput(const BarConfig& base, const WaylandOutput& output);

  // Recursively overlays `overlay` onto `base` (tables merge, everything else
  // replaces). Public so `config validate` can reproduce loadAll's merge order.
  static void deepMerge(toml::table& base, const toml::table& overlay);

private:
  void loadAll();
  [[nodiscard]] static Config makeDefaultConfig();
  static void
  parseConfigTable(const toml::table& tbl, Config& config, bool logSummary, bool logSchemaDiagnostics = true);
  [[nodiscard]] std::optional<Config> configForOverrides(const toml::table& overrides) const;
  [[nodiscard]] noctalia::config::schema::Diagnostics diagnosticsForOverrides(const toml::table& overrides) const;
  [[nodiscard]] bool validateOverrideMutation(
      const toml::table& candidateOverrides, const toml::table* baselineOverrides = nullptr,
      const noctalia::config::schema::Diagnostics* candidateDiagnostics = nullptr
  );
  [[nodiscard]] bool overridePathEffectiveInTable(
      const std::vector<std::string>& path, const toml::table& overrides, const Config* parsedWith = nullptr
  ) const;
  [[nodiscard]] std::size_t overridePreserveDepthForPath(const std::vector<std::string>& path) const;
  void setupWatch();
  // Reconciles inotify watches for [include]d files: watches the parent dir of
  // every loaded file plus every directory named in an [include].files list, and
  // drops watches no longer needed. Safe no-op when hot reload is disabled.
  void refreshIncludeWatches();
  void fireReloadCallbacks();
  void loadOverridesFromFile();
  void setConfigParseError(std::string parseError);
  void updateLegacyConfigIssues(noctalia::config::LegacyConfigIssues issues);
  void notifyLegacyConfigIssues();
  bool writeOverridesToFile();
  void extractWallpaperFromOverrides();
  void extractWallpaperFromTable(const toml::table& table);
  void syncWallpaperFavoritesToOverridesTable();
  [[nodiscard]] bool hasConfiguredWallpaper() const;
  [[nodiscard]] std::string firstRunWallpaperPath() const;

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
  toml::table m_persistedOverridesTable;
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
  noctalia::config::LegacyConfigIssues m_legacyConfigIssues;
  std::string m_loggedLegacyIssueFingerprint;
  bool m_legacyReminderPending = false;
  Timer m_legacyReminderTimer;
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

  // Watches on directories that hold [include]d files (subdirs / absolute paths
  // outside the config dir). Any *.toml change in one of these is a config change.
  // Kept separate from m_symlinkDirWds because matching is dir-wide, not per-file.
  std::unordered_set<int> m_includeDirWds;
  // Canonical directory path -> inotify watch descriptor, for diffing on reload.
  std::unordered_map<std::string, int> m_includeDirWatches;
  // Set by the last loadAll(): every file pulled in (root + included) and every
  // directory named in an [include].files list. Drives refreshIncludeWatches().
  std::vector<std::filesystem::path> m_includeLoadedFiles;
  std::vector<std::filesystem::path> m_includeDirs;

  bool m_ownOverridesWritePending = false;
  std::string m_lastMutationError;
  int m_wallpaperBatchDepth = 0;
  bool m_wallpaperBatchDirty = false;

  ChangeCallback m_wallpaperChangeCallback;
  struct ReloadSubscriber {
    ReloadCallback callback;
    std::string label;
  };
  std::vector<ReloadSubscriber> m_reloadCallbacks;
};
