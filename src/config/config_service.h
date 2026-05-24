#pragma once

#include "config/config_types.h"
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
  [[nodiscard]] bool matchesKeybind(KeybindAction action, std::uint32_t sym, std::uint32_t modifiers) const;
  [[nodiscard]] int watchFd() const noexcept { return m_inotifyFd; }
  [[nodiscard]] std::string buildSupportReport() const;
  [[nodiscard]] std::string buildMergedUserConfig() const;
  [[nodiscard]] std::string buildEffectiveConfig() const;
  [[nodiscard]] bool shouldRunSetupWizard() const;

  void addReloadCallback(ReloadCallback callback);
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

  // Persist a theme-mode override to settings.toml and trigger the reload pipeline.
  void setThemeMode(ThemeMode mode);
  // Persist `[theme].wallpaper_scheme` (palette-from-wallpaper generation) and reload. Returns false if unknown.
  [[nodiscard]] bool setThemeWallpaperScheme(std::string_view scheme);
  // Persist dock enabled override to settings.toml and trigger the reload pipeline.
  void setDockEnabled(bool enabled);
  // Persist desktop widget layout/editor state to settings.toml and trigger the reload pipeline.
  bool setDesktopWidgetsState(const DesktopWidgetsConfig& desktopWidgets);
  bool markSetupWizardCompleted();
  [[nodiscard]] bool hasOverride(const std::vector<std::string>& path) const;
  [[nodiscard]] bool hasEffectiveOverride(const std::vector<std::string>& path) const;
  [[nodiscard]] bool isOverrideOnlyBar(std::string_view name) const;
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
  bool setOverride(const std::vector<std::string>& path, ConfigOverrideValue value);
  bool setOverrides(std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides);
  bool clearOverride(const std::vector<std::string>& path);
  bool renameOverrideTable(const std::vector<std::string>& oldPath, const std::vector<std::string>& newPath);

  [[nodiscard]] static BarConfig resolveForOutput(const BarConfig& base, const WaylandOutput& output);

private:
  static void seedBuiltinWidgets(Config& config);
  static void deepMerge(toml::table& base, const toml::table& overlay);
  void loadAll();
  void parseTableInto(const toml::table& tbl, Config& config, bool logSummary) const;
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

  Config m_config;

  // Hand-authored config directory: all *.toml merged alphabetically.
  std::string m_configDir;

  // App-writable settings file (state dir): lives outside config dir so it
  // can still be written when the config dir is read-only (e.g. NixOS).
  std::string m_overridesPath;
  // Marker file (state dir): its existence means onboarding has been completed
  // or dismissed. Single canonical signal for the setup wizard.
  std::string m_setupMarkerPath;
  toml::table m_overridesTable;
  std::unordered_set<std::string> m_configFileBarNames;
  std::unordered_map<std::string, std::unordered_set<std::string>> m_configFileMonitorOverrideNames;
  std::string m_defaultWallpaperPath;
  std::string m_lastWallpaperPath;
  std::unordered_map<std::string, std::string> m_monitorWallpaperPaths;
  mutable std::unordered_map<std::string, bool> m_effectiveOverrideCache;

  std::string m_overridesParseError;
  std::string m_pendingError; // parse error from initial load, sent as notification once manager is wired up
  uint32_t m_configErrorNotificationId = 0; // ID of the active config-error notification, 0 if none
  NotificationManager* m_notificationManager = nullptr;

  // Single inotify fd, two watch descriptors (config dir + state dir).
  int m_inotifyFd = -1;
  int m_configWatchWd = -1;
  int m_overridesWatchWd = -1;
  // Extra watches on symlink-target directories: wd -> list of filenames to match.
  std::unordered_map<int, std::vector<std::string>> m_symlinkDirWds;

  bool m_ownOverridesWritePending = false;
  int m_wallpaperBatchDepth = 0;
  bool m_wallpaperBatchDirty = false;

  ChangeCallback m_wallpaperChangeCallback;
  std::vector<ReloadCallback> m_reloadCallbacks;
};
