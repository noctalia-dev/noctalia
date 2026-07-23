#include "config/config_service.h"

#include "compositors/compositor_detect.h"
#include "config/atomic_file.h"
#include "config/config_export.h"
#include "config/config_merge.h"
#include "config/config_migrations.h"
#include "config/config_validate.h"
#include "config/schema/config_schema.h"
#include "config/schema/config_sections.h"
#include "config/schema/engine.h"
#include "config/widget_config.h"
#include "core/build_info.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/scoped_timer.h"
#include "i18n/i18n.h"
#include "ipc/ipc_service.h"
#include "launcher/launcher_provider.h"
#include "notification/notification_manager.h"
#include "scripting/plugin_id.h"
#include "shell/desktop/desktop_widget_settings_registry.h"
#include "shell/settings/widget_settings_registry.h"
#include "system/distro_info.h"
#include "system/hardware_info.h"
#include "util/file_utils.h"
#include "util/string_utils.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/inotify.h>
#include <tuple>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace schema = noctalia::config::schema;

namespace {

  constexpr Logger kLog("config");

  template <typename T>
  void readConfigSection(
      const toml::table& table, T& target, const noctalia::config::schema::Schema<T>& sectionSchema,
      std::string_view path, noctalia::config::schema::Diagnostics& diagnostics
  ) {
    T candidate = target;
    try {
      noctalia::config::schema::readInto(table, candidate, sectionSchema, path, diagnostics);
      target = std::move(candidate);
    } catch (const std::exception& e) {
      diagnostics.error(std::string(path), e.what());
      kLog.warn("{}: {}", path, e.what());
    }
  }

  constexpr std::string_view kMigrationReminderOwner = "config_migration_reminders";
  constexpr std::string_view kMigrationReminderKey = "last_notification";
  struct MigrationReminderState {
    std::int64_t epochSeconds = 0;
    std::string issueFingerprint;
  };

  std::optional<MigrationReminderState> parseMigrationReminderState(std::string_view value) {
    const std::size_t separator = value.find('\n');
    if (separator == std::string_view::npos) {
      return std::nullopt;
    }

    std::int64_t epochSeconds = 0;
    const std::string_view encodedEpoch = value.substr(0, separator);
    const auto [end, error] =
        std::from_chars(encodedEpoch.data(), encodedEpoch.data() + encodedEpoch.size(), epochSeconds);
    if (error != std::errc{} || end != encodedEpoch.data() + encodedEpoch.size() || epochSeconds < 0) {
      return std::nullopt;
    }
    return MigrationReminderState{
        .epochSeconds = epochSeconds,
        .issueFingerprint = std::string(value.substr(separator + 1)),
    };
  }

  std::int64_t currentEpochSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  std::optional<double> finiteDouble(const toml::node_view<const toml::node>& node) {
    if (auto v = node.value<double>()) {
      if (!std::isfinite(*v)) {
        return std::nullopt;
      }
      return v;
    }
    if (auto v = node.value<int64_t>()) {
      return static_cast<double>(*v);
    }
    return std::nullopt;
  }

  std::vector<std::string> readStringArray(const toml::node& node) {
    std::vector<std::string> result;
    if (auto* arr = node.as_array()) {
      for (const auto& item : *arr) {
        if (auto* str = item.as_string()) {
          result.push_back(str->get());
        }
      }
    }
    return result;
  }

  // Returns true if `key` is a color-typed setting for `widget`, per the widget
  // setting schema (the single source — not a hand-maintained key list).
  [[nodiscard]] const schema::WidgetSettingField*
  findColorField(const schema::WidgetSettingSchema& fields, std::string_view key) {
    const auto it = std::ranges::find(fields, key, &schema::WidgetSettingField::key);
    if (it == fields.end() || it->type != schema::WidgetSettingType::Color) {
      return nullptr;
    }
    return &*it;
  }

  void validateWidgetColorSettingValue(
      const WidgetSettingValue& value, const std::string& context, bool allowEmpty = false
  ) {
    const auto* raw = std::get_if<std::string>(&value);
    if (raw == nullptr) {
      throw std::runtime_error(context + ": expected string ColorSpec");
    }
    if (StringUtils::trim(*raw).empty()) {
      if (allowEmpty) {
        return;
      }
      throw std::runtime_error(context + ": empty color value is not valid here");
    }
    (void)colorSpecFromConfigString(*raw, context);
  }

  void validateWidgetColorSettings(std::string_view widgetName, const WidgetConfig& widget) {
    const auto fields = settings::widgetSettingSchema(widget.type);
    for (const auto& [key, value] : widget.settings) {
      if (findColorField(fields, key) == nullptr) {
        continue;
      }
      const bool allowEmpty = key == "capsule_border";
      validateWidgetColorSettingValue(value, "widget." + std::string(widgetName) + "." + key, allowEmpty);
    }
  }

  void validateWidgetScaleSetting(std::string_view widgetName, const WidgetConfig& widget) {
    if (!widget.hasSetting("scale")) {
      return;
    }
    (void)resolveWidgetContentScale(1.0f, &widget, "widget." + std::string(widgetName) + ".scale");
  }

  void validateKeyboardLayoutWidgetSettings(std::string_view widgetName, const WidgetConfig& widget) {
    if (widget.type != "keyboard_layout") {
      return;
    }

    const bool showIcon = widget.getBool("show_icon", true);
    const bool showLabel = widget.getBool("show_label", true);
    if (!showIcon && !showLabel) {
      throw std::runtime_error("widget." + std::string(widgetName) + ": show_icon and show_label cannot both be false");
    }
  }

  void validateWidgetSettings(std::string_view widgetName, const WidgetConfig& widget) {
    validateWidgetColorSettings(widgetName, widget);
    validateWidgetScaleSetting(widgetName, widget);
    validateKeyboardLayoutWidgetSettings(widgetName, widget);
  }

  std::optional<std::string> componentOwnerId(std::string_view ownerPath, std::string_view prefix) {
    if (!ownerPath.starts_with(prefix) || ownerPath.size() <= prefix.size()) {
      return std::nullopt;
    }
    return std::string(ownerPath.substr(prefix.size()));
  }

  void restoreInvalidComponents(
      Config& candidate, const Config& active, const noctalia::config::schema::Diagnostics& diagnostics
  ) {
    const auto restorePlacementWidget = [](std::vector<DesktopWidgetState>& candidateWidgets,
                                           const std::vector<DesktopWidgetState>& activeWidgets,
                                           const std::string& id) {
      const auto candidateWidget = std::ranges::find(candidateWidgets, id, &DesktopWidgetState::id);
      const auto activeWidget = std::ranges::find(activeWidgets, id, &DesktopWidgetState::id);
      if (activeWidget != activeWidgets.end()) {
        if (candidateWidget != candidateWidgets.end()) {
          *candidateWidget = *activeWidget;
        } else {
          candidateWidgets.push_back(*activeWidget);
        }
      } else if (candidateWidget != candidateWidgets.end()) {
        candidateWidgets.erase(candidateWidget);
      }
    };

    for (const auto& entry : diagnostics.entries) {
      if (entry.severity != noctalia::config::schema::Diagnostics::Severity::Error
          || entry.recoveryScope != noctalia::config::schema::Diagnostics::RecoveryScope::Component) {
        continue;
      }
      if (const auto barId = componentOwnerId(entry.ownerPath, "widget.")) {
        candidate.widgets.erase(*barId);
        if (const auto activeWidget = active.widgets.find(*barId); activeWidget != active.widgets.end()) {
          candidate.widgets.emplace(*barId, activeWidget->second);
        }
      } else if (const auto desktopId = componentOwnerId(entry.ownerPath, "desktop_widgets.widget.")) {
        restorePlacementWidget(candidate.desktopWidgets.widgets, active.desktopWidgets.widgets, *desktopId);
      } else if (const auto lockscreenId = componentOwnerId(entry.ownerPath, "lockscreen_widgets.widget.")) {
        restorePlacementWidget(candidate.lockscreenWidgets.widgets, active.lockscreenWidgets.widgets, *lockscreenId);
      }
    }
  }

  void validateDesktopWidgetColorSettings(const DesktopWidgetState& widget, std::string_view section) {
    const auto fields = desktop_settings::desktopWidgetSettingSchema(widget.type);
    for (const auto& [key, value] : widget.settings) {
      if (findColorField(fields, key) == nullptr) {
        continue;
      }
      validateWidgetColorSettingValue(value, std::string(section) + ".widget." + widget.id + ".settings." + key);
    }
  }

  DesktopWidgetState
  readDesktopWidgetState(std::string_view id, const toml::table& widgetTable, std::string_view colorSection) {
    DesktopWidgetState widget;
    widget.id = std::string(id);
    if (auto explicitId = widgetTable["id"].value<std::string>()) {
      widget.id = *explicitId;
    }
    if (auto type = widgetTable["type"].value<std::string>()) {
      widget.type = *type;
    }
    if (auto output = widgetTable["output"].value<std::string>()) {
      widget.outputName = *output;
    }
    if (auto cx = finiteDouble(widgetTable["cx"])) {
      widget.cx = static_cast<float>(*cx);
    }
    if (auto cy = finiteDouble(widgetTable["cy"])) {
      widget.cy = static_cast<float>(*cy);
    }
    if (auto boxWidth = finiteDouble(widgetTable["box_width"])) {
      widget.boxWidth = std::max(0.0f, static_cast<float>(*boxWidth));
    }
    if (auto boxHeight = finiteDouble(widgetTable["box_height"])) {
      widget.boxHeight = std::max(0.0f, static_cast<float>(*boxHeight));
    }
    if (auto rotation = finiteDouble(widgetTable["rotation"])) {
      widget.rotationRad = static_cast<float>(*rotation);
    }
    if (auto flipX = widgetTable["flip_x"].value<bool>()) {
      widget.flipX = *flipX;
    }
    if (auto flipY = widgetTable["flip_y"].value<bool>()) {
      widget.flipY = *flipY;
    }
    if (auto enabled = widgetTable["enabled"].value<bool>()) {
      widget.enabled = *enabled;
    }
    if (const auto* settingsTable = widgetTable["settings"].as_table()) {
      for (const auto& [key, value] : *settingsTable) {
        if (auto parsed = noctalia::config::readWidgetSettingValue(value); parsed.has_value()) {
          widget.settings.emplace(std::string(key.str()), std::move(*parsed));
        }
      }
    }
    validateDesktopWidgetColorSettings(widget, colorSection);
    return widget;
  }

  void parseWidgetsPlacementSection(
      const toml::table& sectionTbl, DesktopWidgetsGridState& grid, std::vector<DesktopWidgetState>& widgets,
      std::string_view colorSection
  ) {
    if (const auto* gridTable = sectionTbl["grid"].as_table()) {
      if (auto visible = (*gridTable)["visible"].value<bool>()) {
        grid.visible = *visible;
      }
      if (auto cellSize = (*gridTable)["cell_size"].value<int64_t>()) {
        grid.cellSize = std::clamp(static_cast<std::int32_t>(*cellSize), 8, 256);
      }
      if (auto majorInterval = (*gridTable)["major_interval"].value<int64_t>()) {
        grid.majorInterval = std::clamp(static_cast<std::int32_t>(*majorInterval), 1, 16);
      }
    }
    if (const auto* widgetsTable = sectionTbl["widget"].as_table()) {
      std::vector<DesktopWidgetState> parsedWidgets;
      parsedWidgets.reserve(widgetsTable->size());
      for (const auto& [idNode, widgetNode] : *widgetsTable) {
        const auto* widgetTable = widgetNode.as_table();
        if (widgetTable == nullptr) {
          continue;
        }
        DesktopWidgetState widget;
        try {
          widget = readDesktopWidgetState(idNode.str(), *widgetTable, colorSection);
        } catch (const std::exception& e) {
          kLog.warn("{}.widget.{}: {}", colorSection, idNode.str(), e.what());
          continue;
        }
        if (!widget.id.empty() && !widget.type.empty()) {
          parsedWidgets.push_back(std::move(widget));
        }
      }

      std::vector<std::string> order;
      bool orderSpecified = false;
      if (const auto* orderNode = sectionTbl.get("widget_order")) {
        order = readStringArray(*orderNode);
        orderSpecified = true;
      }

      widgets.clear();
      std::vector<bool> used(parsedWidgets.size(), false);
      for (const auto& orderedId : order) {
        for (std::size_t i = 0; i < parsedWidgets.size(); ++i) {
          if (!used[i] && parsedWidgets[i].id == orderedId) {
            used[i] = true;
            widgets.push_back(std::move(parsedWidgets[i]));
            break;
          }
        }
      }
      if (!orderSpecified) {
        for (std::size_t i = 0; i < parsedWidgets.size(); ++i) {
          if (!used[i]) {
            widgets.push_back(std::move(parsedWidgets[i]));
          }
        }
      }
    }
  }

  const std::vector<KeyChord>& keybindSet(const KeybindsConfig& keybinds, KeybindAction action) {
    switch (action) {
    case KeybindAction::Validate:
      return keybinds.validate;
    case KeybindAction::Cancel:
      return keybinds.cancel;
    case KeybindAction::Left:
      return keybinds.left;
    case KeybindAction::Right:
      return keybinds.right;
    case KeybindAction::Up:
      return keybinds.up;
    case KeybindAction::Down:
      return keybinds.down;
    case KeybindAction::TabNext:
      return keybinds.tabNext;
    case KeybindAction::TabPrevious:
      return keybinds.tabPrevious;
    }
    return keybinds.validate;
  }

  std::vector<std::filesystem::path> sortedConfigTomlFiles(std::string_view configDir) {
    std::vector<std::filesystem::path> files;
    if (configDir.empty()) {
      return files;
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(configDir, ec) || ec) {
      return files;
    }
    for (const auto& entry : std::filesystem::directory_iterator(configDir, ec)) {
      if (entry.is_regular_file() && entry.path().extension() == ".toml") {
        files.push_back(entry.path());
      }
    }
    std::ranges::sort(files);
    return files;
  }

  std::string readTextFile(const std::filesystem::path& path, std::string* error = nullptr) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
      if (error != nullptr) {
        *error = "open failed";
      }
      return {};
    }

    std::ostringstream out;
    out << in.rdbuf();
    if (!in.good() && !in.eof()) {
      if (error != nullptr) {
        *error = "read failed";
      }
      return {};
    }
    if (error != nullptr) {
      error->clear();
    }
    return out.str();
  }

  std::string formatToml(const toml::table& table) {
    std::ostringstream out;
    out << toml::toml_formatter{
        table, toml::toml_formatter::default_flags & ~toml::format_flags::allow_literal_strings
    };
    return out.str();
  }

  std::string utcTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&tt, &tm);

    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
  }

  std::string relativeTo(const std::filesystem::path& path, const std::filesystem::path& base) {
    const auto relative = path.lexically_relative(base);
    if (!relative.empty()) {
      return relative.string();
    }
    return path.filename().string();
  }

  void insertNonEmpty(toml::table& table, std::string_view key, const std::string& value) {
    if (!value.empty()) {
      table.insert_or_assign(std::string(key), value);
    }
  }

  toml::table buildDistroReport() {
    toml::table distro;
    distro.insert_or_assign("label", distroLabel());
    if (const auto detected = DistroDetector::detect(); detected.has_value()) {
      insertNonEmpty(distro, "id", detected->id);
      insertNonEmpty(distro, "name", detected->name);
      insertNonEmpty(distro, "version", detected->version);
      insertNonEmpty(distro, "pretty_name", detected->prettyName);
    }
    return distro;
  }

  toml::table buildCompositorReport() {
    const compositors::CompositorKind kind = compositors::detect();

    toml::table compositor;
    compositor.insert_or_assign("label", compositorLabel());
    compositor.insert_or_assign("name", std::string(compositors::name(kind)));
    const std::string_view hint = compositors::envHint();
    if (!hint.empty()) {
      compositor.insert_or_assign("env_hint", std::string(hint));
    }
    return compositor;
  }

  std::string parseErrorMessage(const std::filesystem::path& path, const toml::parse_error& e) {
    const auto& src = e.source();
    return std::format(
        "{} line {}, column {}: {}", path.filename().string(), src.begin.line, src.begin.column, e.description()
    );
  }

  std::optional<toml::table>
  mergeUserConfigSources(std::string_view configDir, std::string_view settingsPath, std::string* error) {
    auto mergeResult = noctalia::config::mergeConfigWithIncludes(configDir);
    toml::table merged = std::move(mergeResult.merged);
    if (!mergeResult.firstError.empty()) {
      if (error != nullptr) {
        *error = mergeResult.firstError;
        return std::nullopt;
      }
      kLog.warn("skipping config error in merged user config export: {}", mergeResult.firstError);
    }

    if (!settingsPath.empty() && std::filesystem::exists(std::filesystem::path(std::string(settingsPath)))) {
      try {
        toml::table sidecar = toml::parse_file(std::string(settingsPath));
        schema::Diagnostics migrationDiag;
        const auto storedVersion = noctalia::config::storedConfigVersion(sidecar, migrationDiag);
        if (storedVersion.has_value()) {
          (void)noctalia::config::applyPendingConfigMigrations(sidecar, *storedVersion, migrationDiag);
        }
        for (const auto& entry : migrationDiag.entries) {
          if (entry.severity == schema::Diagnostics::Severity::Error) {
            if (error != nullptr) {
              *error = entry.path + ": " + entry.message;
            }
            return std::nullopt;
          }
          kLog.warn("{}: {}", entry.path, entry.message);
        }
        ConfigService::deepMerge(merged, sidecar);
      } catch (const toml::parse_error& e) {
        if (error != nullptr) {
          *error = parseErrorMessage(std::filesystem::path(std::string(settingsPath)), e);
          return std::nullopt;
        }
        kLog.warn("skipping parse error in merged user config export {}: {}", settingsPath, e.description());
      }
    }

    if (error != nullptr) {
      error->clear();
    }
    return merged;
  }

} // namespace

// ── Lifecycle ────────────────────────────────────────────────────────────────

ConfigService::WallpaperBatch::WallpaperBatch(ConfigService& config) : m_config(config) {
  ++m_config.m_wallpaperBatchDepth;
}

ConfigService::WallpaperBatch::~WallpaperBatch() {
  --m_config.m_wallpaperBatchDepth;
  if (m_config.m_wallpaperBatchDepth == 0 && m_config.m_wallpaperBatchDirty) {
    m_config.m_wallpaperBatchDirty = false;
    if (m_config.m_wallpaperChangeCallback) {
      m_config.m_wallpaperChangeCallback();
    }
  }
}

ConfigService::ConfigService() {
  m_configDir = FileUtils::configDir();

  // Resolve settings.toml path; create the state dir eagerly so writes don't
  // race with directory creation later.
  if (auto dir = FileUtils::stateDir(); !dir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    m_overridesPath = dir + "/settings.toml";
    m_stateStore.setPath(dir + "/state.toml");
    m_setupMarkerPath = dir + "/.setup-complete";
  }

  loadOverridesFromFile();
  m_stateStore.load();
  loadAll();
  setupWatch();
}

ConfigService::~ConfigService() {
  if (m_inotifyFd >= 0) {
    if (m_configWatchWd >= 0) {
      inotify_rm_watch(m_inotifyFd, m_configWatchWd);
    }
    if (m_overridesWatchWd >= 0) {
      inotify_rm_watch(m_inotifyFd, m_overridesWatchWd);
    }
    for (const auto& [wd, _] : m_symlinkDirWds) {
      if (wd != m_configWatchWd && wd != m_overridesWatchWd) {
        inotify_rm_watch(m_inotifyFd, wd);
      }
    }
    for (const int wd : m_includeDirWds) {
      inotify_rm_watch(m_inotifyFd, wd);
    }
    ::close(m_inotifyFd);
  }
}

// ── Public interface ─────────────────────────────────────────────────────────

void ConfigService::addReloadCallback(ReloadCallback callback, std::string_view label) {
  m_reloadCallbacks.push_back({std::move(callback), std::string(label)});
}

void ConfigService::setNotificationManager(NotificationManager* manager) {
  m_notificationManager = manager;
  if (m_notificationManager != nullptr && !m_pendingError.empty()) {
    const std::string pendingError = std::move(m_pendingError);
    m_pendingError.clear();
    DeferredCall::callLater([this, pendingError]() {
      if (m_notificationManager == nullptr) {
        m_pendingError = pendingError;
        return;
      }
      if (m_configErrorNotificationId != 0) {
        m_notificationManager->close(m_configErrorNotificationId);
      }
      m_configErrorNotificationId =
          m_notificationManager->addInternal("Noctalia", "Config error", pendingError, Urgency::Critical, 0);
    });
  }
  if (m_notificationManager != nullptr && m_legacyReminderPending) {
    DeferredCall::callLater([this]() { notifyLegacyConfigIssues(); });
  }
}

void ConfigService::forceReload() {
  const auto oldDefault = m_defaultWallpaperPath;
  const auto oldLast = m_lastWallpaperPath;
  const auto oldMonitors = m_monitorWallpaperPaths;

  loadAll();

  const bool wallpaperChanged =
      (oldDefault != m_defaultWallpaperPath
       || oldLast != m_lastWallpaperPath
       || oldMonitors != m_monitorWallpaperPaths);
  if (wallpaperChanged && m_wallpaperChangeCallback) {
    m_wallpaperChangeCallback();
  }
  fireReloadCallbacks();
}

void ConfigService::fireReloadCallbacks() {
  if (!noctalia::profiling::enabled()) {
    for (const auto& sub : m_reloadCallbacks) {
      sub.callback();
    }
    return;
  }

  {
    std::string changed;
    const auto add = [&](bool on, const char* name) {
      if (on) {
        changed += changed.empty() ? name : std::string(", ") + name;
      }
    };
    add(m_lastChange.bars, "bars");
    add(m_lastChange.widgets, "widgets");
    add(m_lastChange.desktopWidgets, "desktopWidgets");
    add(m_lastChange.hotCorners, "hotCorners");
    add(m_lastChange.lockscreenWidgets, "lockscreenWidgets");
    add(m_lastChange.wallpaper, "wallpaper");
    add(m_lastChange.backdrop, "backdrop");
    add(m_lastChange.lockscreen, "lockscreen");
    add(m_lastChange.dock, "dock");
    add(m_lastChange.shell, "shell");
    add(m_lastChange.osd, "osd");
    add(m_lastChange.notification, "notification");
    add(m_lastChange.weather, "weather");
    add(m_lastChange.calendar, "calendar");
    add(m_lastChange.system, "system");
    add(m_lastChange.audio, "audio");
    add(m_lastChange.brightness, "brightness");
    add(m_lastChange.battery, "battery");
    add(m_lastChange.keybinds, "keybinds");
    add(m_lastChange.nightlight, "nightlight");
    add(m_lastChange.location, "location");
    add(m_lastChange.idle, "idle");
    add(m_lastChange.hooks, "hooks");
    add(m_lastChange.theme, "theme");
    add(m_lastChange.controlCenter, "controlCenter");
    add(m_lastChange.plugins, "plugins");
    add(m_lastChange.accessibility, "accessibility");
    kLog.info("reload: changed sections = [{}]", changed.empty() ? "none" : changed);
  }

  noctalia::profiling::StopWatch total;
  for (std::size_t i = 0; i < m_reloadCallbacks.size(); ++i) {
    const auto& sub = m_reloadCallbacks[i];
    noctalia::profiling::StopWatch one;
    sub.callback();
    const double ms = one.elapsedMs();
    if (ms >= 0.5) {
      kLog.info("reload[{}]: {:.1f} ms", sub.label.empty() ? std::format("#{}", i) : sub.label, ms);
    }
  }
  kLog.info("reload: all subscribers {:.1f} ms", total.elapsedMs());
}

bool ConfigService::shouldRunSetupWizard() const {
  if (!m_config.shell.setupWizardEnabled) {
    return false;
  }
  // Single canonical signal: the marker file. If we have no state dir we cannot
  // persist completion, so never show the wizard (it would loop forever).
  return !m_setupMarkerPath.empty() && !std::filesystem::exists(m_setupMarkerPath);
}

std::optional<bool> ConfigService::stateBool(std::string_view owner, std::string_view key) const {
  return m_stateStore.boolValue(owner, key);
}

bool ConfigService::setStateBool(std::string_view owner, std::string_view key, bool value) {
  return m_stateStore.setBool(owner, key, value);
}

std::optional<std::string> ConfigService::stateString(std::string_view owner, std::string_view key) const {
  return m_stateStore.stringValue(owner, key);
}

bool ConfigService::setStateString(std::string_view owner, std::string_view key, std::string_view value) {
  return m_stateStore.setString(owner, key, value);
}

bool ConfigService::clearStateOwner(std::string_view owner) { return m_stateStore.clearOwner(owner); }

std::string ConfigService::buildSupportReport() const {
  toml::table root;

  toml::table report;
  report.insert_or_assign("format_version", std::int64_t{1});
  report.insert_or_assign("generated_by", "noctalia");
  report.insert_or_assign("generated_at_utc", utcTimestamp());
  report.insert_or_assign("noctalia_version", std::string(noctalia::build_info::version()));
  report.insert_or_assign("git_revision", std::string(noctalia::build_info::revision()));
  root.insert_or_assign("report", std::move(report));

  toml::table system;
  system.insert_or_assign("distro", buildDistroReport());
  system.insert_or_assign("compositor", buildCompositorReport());
  root.insert_or_assign("system", std::move(system));

  toml::table paths;
  paths.insert_or_assign("config_dir", m_configDir);
  paths.insert_or_assign("settings_path", m_overridesPath);
  paths.insert_or_assign("state_path", m_stateStore.path().string());
  root.insert_or_assign("paths", std::move(paths));

  toml::table merged;
  toml::array sources;
  const auto configFiles = sortedConfigTomlFiles(m_configDir);
  for (std::size_t i = 0; i < configFiles.size(); ++i) {
    const auto& path = configFiles[i];

    toml::table source;
    source.insert_or_assign("kind", "declarative");
    source.insert_or_assign("load_order", static_cast<std::int64_t>(i));
    source.insert_or_assign("relative_path", relativeTo(path, m_configDir));
    source.insert_or_assign("path", path.string());

    std::string readError;
    source.insert_or_assign("content", readTextFile(path, &readError));
    if (!readError.empty()) {
      source.insert_or_assign("read_error", readError);
    } else {
      try {
        auto table = toml::parse_file(path.string());
        deepMerge(merged, table);
      } catch (const toml::parse_error& e) {
        source.insert_or_assign("parse_error", e.what());
      }
    }

    sources.push_back(std::move(source));
  }
  root.insert_or_assign("config_sources", std::move(sources));

  toml::table state;
  state.insert_or_assign("kind", "state");
  state.insert_or_assign("relative_path", "settings.toml");
  state.insert_or_assign("path", m_overridesPath);

  const bool settingsExists = !m_overridesPath.empty() && std::filesystem::exists(m_overridesPath);
  state.insert_or_assign("exists", settingsExists);
  if (settingsExists) {
    std::string readError;
    state.insert_or_assign("content", readTextFile(m_overridesPath, &readError));
    if (!readError.empty()) {
      state.insert_or_assign("read_error", readError);
    } else {
      try {
        auto table = toml::parse_file(m_overridesPath);
        deepMerge(merged, table);
      } catch (const toml::parse_error& e) {
        state.insert_or_assign("parse_error", e.what());
      }
    }
  } else {
    state.insert_or_assign("content", "");
  }
  root.insert_or_assign("state_settings", std::move(state));

  toml::table mergedConfig;
  mergedConfig.insert_or_assign("content", formatToml(merged));
  root.insert_or_assign("merged_config", std::move(mergedConfig));

  return formatToml(root) + "\n";
}

std::string ConfigService::buildMergedUserConfig() const {
  return buildMergedUserConfigFromSources(m_configDir, m_overridesPath);
}

std::string ConfigService::buildEffectiveConfig() const {
  return formatToml(config_export::serialize(m_config)) + "\n";
}

std::string ConfigService::buildMergedUserConfigFromSources(
    std::string_view configDir, std::string_view settingsPath, std::string* error
) {
  const auto merged = mergeUserConfigSources(configDir, settingsPath, error);
  if (!merged.has_value()) {
    return {};
  }
  toml::table normalized = *merged;
  normalized.erase(noctalia::config::kConfigVersionKey);
  noctalia::config::LegacyConfigIssues issues;
  noctalia::config::normalizeLegacyConfig(normalized, issues);
  return formatToml(normalized) + "\n";
}

std::string ConfigService::buildEffectiveConfigFromSources(
    std::string_view configDir, std::string_view settingsPath, std::string* error
) {
  const auto merged = mergeUserConfigSources(configDir, settingsPath, error);
  if (!merged.has_value()) {
    return {};
  }

  toml::table normalized = *merged;
  normalized.erase(noctalia::config::kConfigVersionKey);
  noctalia::config::LegacyConfigIssues issues;
  noctalia::config::normalizeLegacyConfig(normalized, issues);

  Config config;
  noctalia::config::seedBuiltinWidgets(config);
  if (normalized.empty()) {
    config = makeDefaultConfig();
  } else {
    try {
      parseConfigTable(normalized, config, false, false);
    } catch (const std::exception& e) {
      if (error != nullptr) {
        *error = e.what();
      }
      return {};
    }
  }

  if (error != nullptr) {
    error->clear();
  }
  return formatToml(config_export::serialize(config)) + "\n";
}

Config ConfigService::makeDefaultConfig() {
  Config config;
  noctalia::config::seedBuiltinWidgets(config);
  config.idle.behaviors = defaultIdleBehaviors();
  config.bars.push_back(BarConfig{});
  config.controlCenter.shortcuts = defaultControlCenterShortcuts();
  config.shell.session.actions = defaultSessionPanelActions();
  config.plugins.sources = defaultPluginSources();
  return config;
}

void ConfigService::checkReload() {
  if (m_inotifyFd < 0) {
    return;
  }

  // Drain inotify events and bucket them per watch descriptor.
  alignas(inotify_event) char buf[4096];
  bool configChanged = false;
  bool overridesChanged = false;

  while (true) {
    const auto n = ::read(m_inotifyFd, buf, sizeof(buf));
    if (n <= 0) {
      break;
    }

    std::size_t offset = 0;
    while (offset < static_cast<std::size_t>(n)) {
      auto* event = reinterpret_cast<inotify_event*>(buf + offset);
      if (event->len > 0) {
        const std::string_view name{event->name};
        if (event->wd == m_configWatchWd) {
          if (name.size() >= 5 && name.substr(name.size() - 5) == ".toml") {
            configChanged = true;
          }
        }
        if (event->wd == m_overridesWatchWd) {
          const auto overridesFilename = std::filesystem::path(m_overridesPath).filename().string();
          if (name == overridesFilename) {
            overridesChanged = true;
          }
        }

        // Check whether this event comes from a symlink-target directory.
        const auto symIt = m_symlinkDirWds.find(event->wd);
        if (symIt != m_symlinkDirWds.end()) {
          for (const auto& watched : symIt->second) {
            if (name != watched.filename) {
              continue;
            }
            if (watched.overrides) {
              overridesChanged = true;
            } else {
              configChanged = true;
            }
          }
        }

        // Any *.toml change in a watched [include] directory is a config change.
        if (m_includeDirWds.contains(event->wd) && name.size() >= 5 && name.substr(name.size() - 5) == ".toml") {
          configChanged = true;
        }
      }
      offset += sizeof(inotify_event) + event->len;
    }
  }

  // Skip the echo of our own write.
  if (overridesChanged && m_ownOverridesWritePending) {
    m_ownOverridesWritePending = false;
    overridesChanged = false;
  }

  const auto oldDefault = m_defaultWallpaperPath;
  const auto oldLast = m_lastWallpaperPath;
  const auto oldMonitors = m_monitorWallpaperPaths;

  if (overridesChanged) {
    kLog.info("reloading {}", m_overridesPath);

    loadOverridesFromFile();
    configChanged = true; // overrides affect Config — rebuild it
  }

  if (!configChanged) {
    return;
  }

  kLog.info("config changed, reloading");
  loadAll();
  const bool wallpaperChanged =
      (oldDefault != m_defaultWallpaperPath
       || oldLast != m_lastWallpaperPath
       || oldMonitors != m_monitorWallpaperPaths);
  if (wallpaperChanged && m_wallpaperChangeCallback) {
    m_wallpaperChangeCallback();
  }
  fireReloadCallbacks();
}

BarConfig ConfigService::resolveForOutput(const BarConfig& base, const WaylandOutput& output) {
  BarConfig resolved = base;

  for (const auto& ovr : base.monitorOverrides) {
    if (!outputMatchesSelector(ovr.match, output)) {
      continue;
    }

    kLog.debug("monitor override \"{}\" matched output {} ({})", ovr.match, output.connectorName, output.description);

    if (ovr.position)
      resolved.position = *ovr.position;
    if (ovr.enabled)
      resolved.enabled = *ovr.enabled;
    if (ovr.autoHide)
      resolved.autoHide = *ovr.autoHide;
    if (ovr.smartAutoHide)
      resolved.smartAutoHide = *ovr.smartAutoHide;
    if (ovr.showOnWorkspaceSwitch)
      resolved.showOnWorkspaceSwitch = *ovr.showOnWorkspaceSwitch;
    if (ovr.reserveSpace)
      resolved.reserveSpace = *ovr.reserveSpace;
    if (ovr.layer)
      resolved.layer = *ovr.layer;
    if (ovr.thickness)
      resolved.thickness = *ovr.thickness;
    if (ovr.backgroundOpacity)
      resolved.backgroundOpacity = *ovr.backgroundOpacity;
    if (ovr.border)
      resolved.border = *ovr.border;
    if (ovr.borderWidth)
      resolved.borderWidth = *ovr.borderWidth;
    if (ovr.radius) {
      resolved.radius = *ovr.radius;
      resolved.radiusTopLeft = *ovr.radius;
      resolved.radiusTopRight = *ovr.radius;
      resolved.radiusBottomLeft = *ovr.radius;
      resolved.radiusBottomRight = *ovr.radius;
    }
    if (ovr.radiusTopLeft)
      resolved.radiusTopLeft = *ovr.radiusTopLeft;
    if (ovr.radiusTopRight)
      resolved.radiusTopRight = *ovr.radiusTopRight;
    if (ovr.radiusBottomLeft)
      resolved.radiusBottomLeft = *ovr.radiusBottomLeft;
    if (ovr.radiusBottomRight)
      resolved.radiusBottomRight = *ovr.radiusBottomRight;
    if (ovr.concaveEdgeCorners)
      resolved.concaveEdgeCorners = *ovr.concaveEdgeCorners;
    if (ovr.marginEnds)
      resolved.marginEnds = *ovr.marginEnds;
    if (ovr.marginEdge)
      resolved.marginEdge = *ovr.marginEdge;
    if (ovr.marginOppositeEdge)
      resolved.marginOppositeEdge = *ovr.marginOppositeEdge;
    if (ovr.padding)
      resolved.padding = *ovr.padding;
    if (ovr.widgetSpacing)
      resolved.widgetSpacing = *ovr.widgetSpacing;
    if (ovr.shadow)
      resolved.shadow = *ovr.shadow;
    if (ovr.contactShadow)
      resolved.contactShadow = *ovr.contactShadow;
    if (ovr.panelOverlap)
      resolved.panelOverlap = *ovr.panelOverlap;
    if (ovr.capsuleThickness)
      resolved.capsuleThickness = *ovr.capsuleThickness;
    if (ovr.fontFamily)
      resolved.fontFamily = *ovr.fontFamily;
    if (ovr.startWidgets)
      resolved.startWidgets = *ovr.startWidgets;
    if (ovr.centerWidgets)
      resolved.centerWidgets = *ovr.centerWidgets;
    if (ovr.endWidgets)
      resolved.endWidgets = *ovr.endWidgets;
    if (ovr.scale)
      resolved.scale = *ovr.scale;
    if (ovr.widgetCapsuleDefault)
      resolved.widgetCapsuleDefault = *ovr.widgetCapsuleDefault;
    if (ovr.widgetCapsuleFill)
      resolved.widgetCapsuleFill = *ovr.widgetCapsuleFill;
    if (ovr.widgetCapsuleBorderSpecified) {
      resolved.widgetCapsuleBorderSpecified = true;
      resolved.widgetCapsuleBorder = ovr.widgetCapsuleBorder;
    }
    if (ovr.widgetCapsuleForeground) {
      resolved.widgetCapsuleForeground = *ovr.widgetCapsuleForeground;
    }
    if (ovr.widgetColor) {
      resolved.widgetColor = *ovr.widgetColor;
    }
    if (ovr.widgetIconColor) {
      resolved.widgetIconColor = *ovr.widgetIconColor;
    }
    if (ovr.widgetCapsuleGroups) {
      resolved.widgetCapsuleGroups = *ovr.widgetCapsuleGroups;
    }
    if (ovr.widgetCapsulePadding) {
      resolved.widgetCapsulePadding = std::clamp(static_cast<float>(*ovr.widgetCapsulePadding), 0.0f, 48.0f);
    }
    if (ovr.widgetCapsuleRadius.has_value()) {
      resolved.widgetCapsuleRadius = std::clamp(*ovr.widgetCapsuleRadius, 0.0, 80.0);
    }
    if (ovr.widgetCapsuleOpacity) {
      resolved.widgetCapsuleOpacity = std::clamp(static_cast<float>(*ovr.widgetCapsuleOpacity), 0.0f, 1.0f);
    }
    if (ovr.hoverHighlight) {
      resolved.hoverHighlight = *ovr.hoverHighlight;
    }
    if (ovr.deadZone.command) {
      resolved.deadZone.command = *ovr.deadZone.command;
    }
    if (ovr.deadZone.rightCommand) {
      resolved.deadZone.rightCommand = *ovr.deadZone.rightCommand;
    }
    if (ovr.deadZone.middleCommand) {
      resolved.deadZone.middleCommand = *ovr.deadZone.middleCommand;
    }
    if (ovr.deadZone.scrollUpCommand) {
      resolved.deadZone.scrollUpCommand = *ovr.deadZone.scrollUpCommand;
    }
    if (ovr.deadZone.scrollDownCommand) {
      resolved.deadZone.scrollDownCommand = *ovr.deadZone.scrollDownCommand;
    }
    break; // first match wins
  }

  return resolved;
}

// ── Private helpers ──────────────────────────────────────────────────────────

void ConfigService::setupWatch() {
  if (m_configDir.empty()) {
    return;
  }

  std::error_code ec;
  std::filesystem::create_directories(m_configDir, ec);

  m_inotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (m_inotifyFd < 0) {
    kLog.warn("inotify_init1 failed, hot reload disabled");
    return;
  }

  m_configWatchWd =
      inotify_add_watch(m_inotifyFd, m_configDir.c_str(), IN_MODIFY | IN_CLOSE_WRITE | IN_CREATE | IN_MOVED_TO);
  if (m_configWatchWd < 0) {
    kLog.warn("inotify_add_watch failed, hot reload disabled");
    ::close(m_inotifyFd);
    m_inotifyFd = -1;
    return;
  }

  kLog.debug("watching {} for changes", m_configDir);

  // For any *.toml entries that are symlinks, also watch the real target's parent
  // directory so that edits to the target file (e.g. via dotfile management) trigger
  // a reload even though the modification event fires in a different directory.
  {
    std::error_code scanEc;
    for (const auto& entry : std::filesystem::directory_iterator(m_configDir, scanEc)) {
      if (entry.path().extension() != ".toml") {
        continue;
      }
      std::error_code symlinkEc;
      if (!entry.is_symlink(symlinkEc) || symlinkEc) {
        continue;
      }
      std::error_code canonEc;
      const auto real = std::filesystem::canonical(entry.path(), canonEc);
      if (canonEc) {
        continue;
      }
      const auto realDir = real.parent_path().string();
      const auto realName = real.filename().string();
      // inotify_add_watch is idempotent per inode — if realDir == m_configDir the
      // existing watch descriptor is returned and we simply record the extra name.
      const int wd =
          inotify_add_watch(m_inotifyFd, realDir.c_str(), IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
      if (wd >= 0) {
        m_symlinkDirWds[wd].push_back(SymlinkTargetWatch{.filename = realName, .overrides = false});
        kLog.debug("watching symlink target {} in {}", realName, realDir);
      }
    }
  }

  // Also watch the state dir for settings.toml edits (external writes).
  if (!m_overridesPath.empty()) {
    const auto overridesDir = std::filesystem::path(m_overridesPath).parent_path().string();
    m_overridesWatchWd =
        inotify_add_watch(m_inotifyFd, overridesDir.c_str(), IN_MODIFY | IN_CLOSE_WRITE | IN_CREATE | IN_MOVED_TO);
    if (m_overridesWatchWd < 0) {
      kLog.warn("inotify_add_watch failed for {}, overrides reload disabled", overridesDir);
    } else {
      kLog.debug("watching {} for changes", overridesDir);
    }

    const auto target = resolveAtomicWriteTarget(m_overridesPath);
    if (target.has_value() && target->throughSymlink) {
      const auto realDir = target->path.parent_path().string();
      const auto realName = target->path.filename().string();
      const int wd =
          inotify_add_watch(m_inotifyFd, realDir.c_str(), IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
      if (wd >= 0) {
        m_symlinkDirWds[wd].push_back(SymlinkTargetWatch{.filename = realName, .overrides = true});
        kLog.debug("watching settings symlink target {} in {}", realName, realDir);
      }
    }
  }

  // The ctor's first loadAll() ran before this fd existed, so establish the
  // initial [include] directory watches now.
  refreshIncludeWatches();
}

void ConfigService::refreshIncludeWatches() {
  if (m_inotifyFd < 0) {
    return;
  }

  const auto canonical = [](const std::filesystem::path& p) {
    std::error_code ec;
    const auto c = std::filesystem::weakly_canonical(p, ec);
    return (ec ? p.lexically_normal() : c).string();
  };

  // Desired = parent dir of every loaded file + every directory named in an
  // [include].files list, minus dirs already covered by the primary watches.
  std::unordered_set<std::string> desired;
  for (const auto& file : m_includeLoadedFiles) {
    auto dir = canonical(file.parent_path());
    if (!dir.empty()) {
      desired.insert(std::move(dir));
    }
  }
  for (const auto& dir : m_includeDirs) {
    auto canon = canonical(dir);
    if (!canon.empty()) {
      desired.insert(std::move(canon));
    }
  }
  if (!m_configDir.empty()) {
    desired.erase(canonical(std::filesystem::path(m_configDir)));
  }
  if (!m_overridesPath.empty()) {
    desired.erase(canonical(std::filesystem::path(m_overridesPath).parent_path()));
  }

  // Drop watches no longer wanted.
  for (auto it = m_includeDirWatches.begin(); it != m_includeDirWatches.end();) {
    if (!desired.contains(it->first)) {
      inotify_rm_watch(m_inotifyFd, it->second);
      m_includeDirWds.erase(it->second);
      it = m_includeDirWatches.erase(it);
    } else {
      ++it;
    }
  }

  // Add newly wanted watches.
  for (const auto& dir : desired) {
    if (m_includeDirWatches.contains(dir)) {
      continue;
    }
    const int wd = inotify_add_watch(m_inotifyFd, dir.c_str(), IN_MODIFY | IN_CLOSE_WRITE | IN_CREATE | IN_MOVED_TO);
    if (wd < 0) {
      continue;
    }
    // inotify_add_watch is idempotent per inode: if this dir is already watched by
    // the config/overrides/symlink-target watches, the existing wd comes back. Do
    // not record it (and never remove it) — its original owner manages its lifetime.
    if (wd == m_configWatchWd || wd == m_overridesWatchWd || m_symlinkDirWds.contains(wd)) {
      continue;
    }
    m_includeDirWatches.emplace(dir, wd);
    m_includeDirWds.insert(wd);
    kLog.debug("watching include directory {}", dir);
  }
}

void ConfigService::loadOverridesFromFile() {
  m_overridesTable = toml::table{};
  m_defaultWallpaperPath.clear();
  m_lastWallpaperPath.clear();
  m_monitorWallpaperPaths.clear();
  m_overridesParseError.clear();

  if (m_overridesPath.empty() || !std::filesystem::exists(m_overridesPath)) {
    m_persistedOverridesTable = toml::table{};
    return;
  }

  kLog.info("loading {}", m_overridesPath);
  try {
    m_overridesTable = toml::parse_file(m_overridesPath);
    m_persistedOverridesTable = m_overridesTable;
  } catch (const toml::parse_error& e) {
    const auto& src = e.source();
    kLog.warn(
        "parse error in {} at line {}, column {}: {}", m_overridesPath, src.begin.line, src.begin.column,
        e.description()
    );
    m_overridesParseError = std::format(
        "{} line {}, column {}: {}", std::filesystem::path(m_overridesPath).filename().string(), src.begin.line,
        src.begin.column, e.description()
    );
    m_overridesTable = toml::table{};
    return;
  }
  extractWallpaperFromOverrides();
}

void ConfigService::setConfigParseError(std::string parseError) {
  if (parseError.empty()) {
    // Dismiss any previous config-error notification.
    if (m_notificationManager != nullptr && m_configErrorNotificationId != 0) {
      m_notificationManager->close(m_configErrorNotificationId);
      m_configErrorNotificationId = 0;
    }
    m_pendingError.clear();
    return;
  }

  if (m_notificationManager != nullptr) {
    if (m_configErrorNotificationId != 0) {
      m_notificationManager->close(m_configErrorNotificationId);
    }
    m_configErrorNotificationId =
        m_notificationManager->addInternal("Noctalia", "Config error", parseError, Urgency::Critical, 0);
  } else {
    m_pendingError = std::move(parseError);
  }
}

void ConfigService::updateLegacyConfigIssues(noctalia::config::LegacyConfigIssues issues) {
  std::ranges::sort(issues, [](const auto& lhs, const auto& rhs) {
    return std::tie(lhs.migrationVersion, lhs.path) < std::tie(rhs.migrationVersion, rhs.path);
  });
  issues.erase(
      std::ranges::unique(
          issues, {}, [](const auto& issue) { return std::tie(issue.migrationVersion, issue.path); }
      ).begin(),
      issues.end()
  );

  const std::string fingerprint = noctalia::config::legacyConfigIssueFingerprint(issues);
  if (fingerprint != m_loggedLegacyIssueFingerprint) {
    for (const auto& issue : issues) {
      kLog.warn("{}: {} (migrated in memory)", issue.path, issue.message);
    }
    m_loggedLegacyIssueFingerprint = fingerprint;
  }
  m_legacyConfigIssues = std::move(issues);

  if (m_legacyConfigIssues.empty()) {
    m_legacyReminderPending = false;
    m_legacyReminderTimer.stop();
    if (m_stateStore.stringValue(kMigrationReminderOwner, kMigrationReminderKey).has_value()) {
      (void)m_stateStore.clearOwner(kMigrationReminderOwner);
    }
    return;
  }

  const std::int64_t now = currentEpochSeconds();
  const auto encodedState = m_stateStore.stringValue(kMigrationReminderOwner, kMigrationReminderKey);
  const auto reminderState = encodedState.has_value() ? parseMigrationReminderState(*encodedState) : std::nullopt;
  const bool hasNewIssues = !reminderState.has_value()
      || noctalia::config::legacyConfigFingerprintHasNewIssues(fingerprint, reminderState->issueFingerprint);
  const bool intervalElapsed = !reminderState.has_value()
      || noctalia::config::legacyConfigReminderIntervalElapsed(now, reminderState->epochSeconds);
  m_legacyReminderPending = hasNewIssues || intervalElapsed;
  if (m_legacyReminderPending) {
    notifyLegacyConfigIssues();
    return;
  }

  const auto elapsed = std::chrono::seconds(now - reminderState->epochSeconds);
  const auto remaining = std::chrono::seconds(noctalia::config::kLegacyConfigReminderIntervalSeconds) - elapsed;
  m_legacyReminderTimer.start(std::chrono::duration_cast<std::chrono::milliseconds>(remaining), [this]() {
    m_legacyReminderPending = true;
    notifyLegacyConfigIssues();
  });
}

void ConfigService::notifyLegacyConfigIssues() {
  if (!m_legacyReminderPending || m_notificationManager == nullptr || m_legacyConfigIssues.empty()) {
    return;
  }

  const std::string fingerprint = noctalia::config::legacyConfigIssueFingerprint(m_legacyConfigIssues);
  const std::int64_t now = currentEpochSeconds();
  (void)m_notificationManager->addInternal(
      "Noctalia", i18n::tr("notifications.internal.config-migration-title"),
      i18n::tr("notifications.internal.config-migration-body", "path", m_legacyConfigIssues.front().path),
      Urgency::Normal
  );
  (void)m_stateStore.setString(kMigrationReminderOwner, kMigrationReminderKey, std::format("{}\n{}", now, fingerprint));
  m_legacyReminderPending = false;
  m_legacyReminderTimer.start(
      std::chrono::milliseconds(noctalia::config::kLegacyConfigReminderIntervalSeconds * 1000), [this]() {
        m_legacyReminderPending = true;
        notifyLegacyConfigIssues();
      }
  );
}

void ConfigService::deepMerge(toml::table& base, const toml::table& overlay) {
  for (const auto& [k, v] : overlay) {
    if (const auto* overlayTbl = v.as_table()) {
      if (auto* baseNode = base.get(k)) {
        if (auto* baseTbl = baseNode->as_table()) {
          deepMerge(*baseTbl, *overlayTbl);
          continue;
        }
      }
    }
    // Tables-over-non-tables, non-tables, and arrays: overlay replaces base wholesale.
    base.insert_or_assign(k, v);
  }
}

void ConfigService::loadAll() {
  noctalia::profiling::ScopedTimer parseTimer(kLog, "reload: parse (loadAll)");
  m_effectiveOverrideCache.clear();

  Config nextConfig;
  noctalia::config::seedBuiltinWidgets(nextConfig);

  auto mergeResult = noctalia::config::mergeConfigWithIncludes(m_configDir);
  toml::table merged = std::move(mergeResult.merged);
  std::string firstError = std::move(mergeResult.firstError);
  m_includeLoadedFiles = std::move(mergeResult.loadedFiles);
  m_includeDirs = std::move(mergeResult.includeDirs);

  decltype(m_configFileBarNames) configFileBarNames;
  decltype(m_configFileMonitorOverrideNames) configFileMonitorOverrideNames;
  decltype(m_configFileCalendarAccountNames) configFileCalendarAccountNames;
  if (auto* barTblMap = merged["bar"].as_table()) {
    for (const auto& [barName, barNode] : *barTblMap) {
      auto* barTbl = barNode.as_table();
      if (barTbl == nullptr) {
        continue;
      }
      const std::string barNameStr(barName.str());
      configFileBarNames.insert(barNameStr);
      if (auto* monTblMap = (*barTbl)["monitor"].as_table()) {
        auto& monitorNames = configFileMonitorOverrideNames[barNameStr];
        for (const auto& [monName, monNode] : *monTblMap) {
          auto* monTbl = monNode.as_table();
          if (monTbl == nullptr) {
            continue;
          }
          if (auto match = (*monTbl)["match"].value<std::string>()) {
            monitorNames.insert(*match);
          } else {
            monitorNames.insert(std::string(monName.str()));
          }
        }
      }
    }
  }
  if (auto* calendarTbl = merged["calendar"].as_table()) {
    if (auto* accountTblMap = (*calendarTbl)["account"].as_table()) {
      for (const auto& [accountName, accountNode] : *accountTblMap) {
        if (accountNode.as_table() != nullptr) {
          configFileCalendarAccountNames.insert(std::string(accountName.str()));
        }
      }
    }
  }

  toml::table effectiveOverrides = m_overridesTable;
  schema::Diagnostics migrationDiag;
  std::string migrationError;
  int storedVersion = noctalia::config::currentConfigVersion();
  int appliedVersion = storedVersion;
  bool sidecarNeedsPersist = false;
  if (!m_overridesTable.empty()) {
    const auto parsedVersion = noctalia::config::storedConfigVersion(effectiveOverrides, migrationDiag);
    if (parsedVersion.has_value()) {
      storedVersion = *parsedVersion;
      appliedVersion = noctalia::config::applyPendingConfigMigrations(effectiveOverrides, storedVersion, migrationDiag);
      sidecarNeedsPersist = appliedVersion != storedVersion;
      effectiveOverrides.insert_or_assign(
          noctalia::config::kConfigVersionKey, static_cast<std::int64_t>(appliedVersion)
      );
    }
  }
  for (const auto& entry : migrationDiag.entries) {
    if (entry.severity == schema::Diagnostics::Severity::Error) {
      if (migrationError.empty()) {
        migrationError = entry.path + ": " + entry.message;
      }
    } else {
      kLog.warn("{}: {}", entry.path, entry.message);
    }
  }

  // Apply the app-writable overrides overlay last; sidecar wins. Compatibility
  // normalization must see the final effective values to preserve overlay intent.
  deepMerge(merged, effectiveOverrides);
  merged.erase(noctalia::config::kConfigVersionKey);
  noctalia::config::LegacyConfigIssues legacyIssues;
  noctalia::config::normalizeLegacyConfig(merged, legacyIssues);

  if (m_includeLoadedFiles.empty() && m_overridesTable.empty()) {
    kLog.info("no config files found, using defaults");
    m_lastChange = ConfigChangeSet{};
    m_config = makeDefaultConfig();
    m_configFileBarNames.clear();
    m_configFileMonitorOverrideNames.clear();
    m_configFileCalendarAccountNames.clear();
    m_defaultWallpaperPath.clear();
    m_lastWallpaperPath.clear();
    m_monitorWallpaperPaths.clear();
    updateLegacyConfigIssues({});
    setConfigParseError(m_overridesParseError);
    refreshIncludeWatches();
    return;
  }

  std::string semanticError = !firstError.empty() ? firstError
      : !m_overridesParseError.empty()            ? m_overridesParseError
                                                  : migrationError;
  std::string diagnosticError;
  schema::Diagnostics diagnostics;
  if (semanticError.empty()) {
    try {
      diagnostics = noctalia::config::validateMergedConfig(merged);
      std::size_t errorCount = 0;
      for (const auto& entry : diagnostics.entries) {
        if (entry.severity == schema::Diagnostics::Severity::Error) {
          if (entry.recoveryScope == schema::Diagnostics::RecoveryScope::Document) {
            if (semanticError.empty()) {
              semanticError = entry.path + ": " + entry.message;
            }
            kLog.warn("{}: {}", entry.path, entry.message);
            continue;
          }
          ++errorCount;
          if (diagnosticError.empty()) {
            diagnosticError = entry.path + ": " + entry.message;
          }
        }
        kLog.warn("{}: {}", entry.path, entry.message);
      }
      if (errorCount > 1) {
        diagnosticError += std::format(" (and {} more config errors)", errorCount - 1);
      }
    } catch (const std::exception& e) {
      semanticError = e.what();
      kLog.warn("config validation error: {}", semanticError);
    }
  }
  if (semanticError.empty()) {
    try {
      parseConfigTable(merged, nextConfig, true, false);
      restoreInvalidComponents(nextConfig, m_config, diagnostics);
    } catch (const std::exception& e) {
      semanticError = e.what();
      kLog.warn("config parse error: {}", semanticError);
    }
  }

  if (semanticError.empty()) {
    m_lastChange = computeConfigChangeSet(m_config, nextConfig);
    m_config = std::move(nextConfig);
    m_configFileBarNames = std::move(configFileBarNames);
    m_configFileMonitorOverrideNames = std::move(configFileMonitorOverrideNames);
    m_configFileCalendarAccountNames = std::move(configFileCalendarAccountNames);
    extractWallpaperFromTable(merged);
    updateLegacyConfigIssues(std::move(legacyIssues));

    if (sidecarNeedsPersist) {
      toml::table previousOverrides = m_overridesTable;
      m_overridesTable = std::move(effectiveOverrides);
      if (writeOverridesToFile()) {
        m_ownOverridesWritePending = m_inotifyFd >= 0 && m_overridesWatchWd >= 0;
        extractWallpaperFromOverrides();
      } else {
        kLog.warn("failed to persist migrated config overrides to {}", m_overridesPath);
        m_overridesTable = std::move(previousOverrides);
      }
    }
  } else if (m_config.bars.empty()) {
    m_lastChange = ConfigChangeSet{};
    m_config = makeDefaultConfig();
    m_configFileBarNames.clear();
    m_configFileMonitorOverrideNames.clear();
    m_configFileCalendarAccountNames.clear();
    m_defaultWallpaperPath.clear();
    m_lastWallpaperPath.clear();
    m_monitorWallpaperPaths.clear();
  } else {
    // Parse error with a usable previous config retained — fan out conservatively.
    m_lastChange = ConfigChangeSet{};
  }

  const std::string parseError = !firstError.empty() ? firstError
      : !m_overridesParseError.empty()               ? m_overridesParseError
      : !semanticError.empty()                       ? semanticError
                                                     : diagnosticError;
  setConfigParseError(parseError);

  // Included files may live in subdirectories or absolute paths outside the config
  // dir, and the include set can change on every reload — reconcile their watches.
  refreshIncludeWatches();
}

void ConfigService::parseConfigTable(
    const toml::table& tbl, Config& config, bool logSummary, bool logSchemaDiagnostics
) {
  // Diagnostics raised by schema-driven sections (e.g. unknown enum values).
  // Flushed to the log below, preserving the legacy warn-and-continue behavior.
  schema::Diagnostics schemaDiag;

  // Parse [bar.*] named subtables
  if (auto* barTblMap = tbl["bar"].as_table()) {
    std::vector<BarConfig> parsedBars;
    for (const auto& [barName, barNode] : *barTblMap) {
      auto* barTbl = barNode.as_table();
      if (barTbl == nullptr) {
        continue;
      }

      BarConfig bar;
      bar.name = std::string(barName.str());
      // position is read explicitly (the base bar always emits it; monitor
      // overrides emit it conditionally), the rest via the shared schema.
      if (auto v = (*barTbl)["position"].value<std::string>()) {
        bar.position = *v;
      }
      readConfigSection(*barTbl, bar, schema::barFieldsSchema(), "bar." + bar.name, schemaDiag);

      // Parse [bar.<name>.monitor.*] overrides — insertion order preserved by toml++.
      if (auto* monTblMap = (*barTbl)["monitor"].as_table()) {
        for (const auto& [monName, monNode] : *monTblMap) {
          auto* monTbl = monNode.as_table();
          if (monTbl == nullptr) {
            continue;
          }
          BarMonitorOverride ovr;
          ovr.match = std::string(monName.str()); // key is the match unless an explicit `match` overrides it
          readConfigSection(
              *monTbl, ovr, schema::barMonitorOverrideSchema(),
              "bar." + bar.name + ".monitor." + std::string(monName.str()), schemaDiag
          );
          bar.monitorOverrides.push_back(std::move(ovr));
        }
      }

      parsedBars.push_back(std::move(bar));
    }

    std::vector<std::string> order;
    if (auto* orderNode = (*barTblMap)["order"].as_array()) {
      order = readStringArray(*orderNode);
    }

    std::vector<bool> used(parsedBars.size(), false);
    for (const auto& orderedName : order) {
      for (std::size_t i = 0; i < parsedBars.size(); ++i) {
        if (!used[i] && parsedBars[i].name == orderedName) {
          used[i] = true;
          config.bars.push_back(std::move(parsedBars[i]));
          break;
        }
      }
    }

    for (std::size_t i = 0; i < parsedBars.size(); ++i) {
      if (!used[i]) {
        config.bars.push_back(std::move(parsedBars[i]));
      }
    }
  }

  // Parse [widget.*] — named widget instances with per-widget settings
  if (auto* widgetTbl = tbl["widget"].as_table()) {
    for (const auto& [name, node] : *widgetTbl) {
      auto* entryTbl = node.as_table();
      if (entryTbl == nullptr) {
        continue;
      }

      const std::string widgetName(name.str());
      WidgetConfig wc = noctalia::config::readBarWidgetConfig(widgetName, *entryTbl, config);

      try {
        validateWidgetSettings(widgetName, wc);
        config.widgets[widgetName] = std::move(wc);
      } catch (const std::exception& e) {
        config.widgets.erase(widgetName);
        kLog.warn("widget.{}: {}", widgetName, e.what());
      }
    }
  }

  // Every schema-backed section is read through the section registry, so the loader
  // cannot recognize a section that the validator and exporter do not.
  for (const schema::SectionSpec& spec : schema::sections()) {
    const auto* sectionTbl = tbl[spec.name].as_table();
    if (sectionTbl == nullptr) {
      continue;
    }
    try {
      spec.read(*sectionTbl, config, schemaDiag);
    } catch (const std::exception& e) {
      schemaDiag.error(std::string(spec.name), e.what());
      kLog.warn("{}: {}", spec.name, e.what());
    }
  }

  // Template config files (e.g. user-templates.toml) store palette extensions under
  // [config.custom_colors]; lift them into the canonical theme.templates slot.
  schema::liftTemplateConfigCustomColors(tbl, config);

  // Default seeding must apply even when the section is absent, so it runs after the
  // registry pass. A schema read can't tell an explicitly empty list from a missing
  // one, so these probe the raw table for the list key.
  const auto hasExplicitArray = [&tbl](std::string_view section, std::string_view key) {
    const auto* sectionTbl = tbl[section].as_table();
    return sectionTbl != nullptr && (*sectionTbl)[key].as_array() != nullptr;
  };
  const bool sessionActionsConfigured = [&tbl] {
    const auto* shellTbl = tbl["shell"].as_table();
    const auto* sessionTbl = shellTbl != nullptr ? (*shellTbl)["session"].as_table() : nullptr;
    return sessionTbl != nullptr && (*sessionTbl)["actions"].as_array() != nullptr;
  }();
  if (!sessionActionsConfigured && config.shell.session.actions.empty()) {
    config.shell.session.actions = defaultSessionPanelActions();
  }
  if (!hasExplicitArray("control_center", "shortcuts") && config.controlCenter.shortcuts.empty()) {
    config.controlCenter.shortcuts = defaultControlCenterShortcuts();
  }
  if (!hasExplicitArray("plugins", "source") && config.plugins.sources.empty()) {
    config.plugins.sources = defaultPluginSources();
  }
  if (config.idle.behaviors.empty()) {
    config.idle.behaviors = defaultIdleBehaviors();
  }

  // Launcher providers are resolved after the registry pass, once config.shell is
  // populated. An empty common prefix falls back to '/', and providers that name
  // nothing real (or a disabled plugin, or the fixed Applications provider) are
  // dropped loudly rather than silently ignored.
  if (config.shell.launcher.providerPrefix.empty()) {
    schemaDiag.warn("shell.launcher.provider_prefix", "is empty, falling back to '/'");
    config.shell.launcher.providerPrefix = "/";
  }
  std::unordered_set<std::string> enabledPlugins;
  if (const auto* pluginsTbl = tbl["plugins"].as_table()) {
    if (const auto* enabledArr = (*pluginsTbl)["enabled"].as_array()) {
      for (const auto& node : *enabledArr) {
        if (const auto* strVal = node.as_string()) {
          enabledPlugins.insert(StringUtils::toLower(strVal->get()));
        }
      }
    }
  }
  std::erase_if(config.shell.launcher.providers, [&](const LauncherProviderConfig& provider) {
    if (provider.name == "applications") {
      schemaDiag.warn(
          "shell.launcher.providers.applications", "custom settings are not allowed (Applications is always global)"
      );
      return true;
    }
    const auto isBuiltin = std::ranges::contains(launcher::kBuiltinProviders, provider.name);
    if (!isBuiltin) {
      const std::size_t colon = provider.name.find(':');
      bool isPlugin = false;
      std::string pluginIdStr;
      if (colon != std::string::npos) {
        std::string_view pluginId = std::string_view(provider.name).substr(0, colon);
        std::string_view entryName = std::string_view(provider.name).substr(colon + 1);
        if (scripting::isValidPluginId(pluginId) && scripting::isValidPluginIdSegment(entryName)) {
          isPlugin = true;
          pluginIdStr = std::string(pluginId);
        }
      }
      if (!isPlugin) {
        schemaDiag.warn("shell.launcher.providers." + provider.name, "provider is nonexistent");
        return true;
      }
      if (!enabledPlugins.contains(pluginIdStr)) {
        schemaDiag.warn("shell.launcher.providers." + provider.name, "plugin '" + pluginIdStr + "' is not enabled");
        return true;
      }
    }
    return false;
  });

  // Parse [desktop_widgets]
  if (auto* desktopWidgetsTbl = tbl["desktop_widgets"].as_table()) {
    auto& desktopWidgets = config.desktopWidgets;
    if (auto v = (*desktopWidgetsTbl)["enabled"].value<bool>()) {
      desktopWidgets.enabled = *v;
    }
    if (auto schemaVersion = (*desktopWidgetsTbl)["schema_version"].value<int64_t>()) {
      desktopWidgets.schemaVersion = static_cast<std::int32_t>(*schemaVersion);
    }
    parseWidgetsPlacementSection(*desktopWidgetsTbl, desktopWidgets.grid, desktopWidgets.widgets, "desktop_widgets");
  }

  // Parse [lockscreen_widgets]
  if (auto* lockscreenWidgetsTbl = tbl["lockscreen_widgets"].as_table()) {
    auto& lockscreenWidgets = config.lockscreenWidgets;
    if (auto v = (*lockscreenWidgetsTbl)["enabled"].value<bool>()) {
      lockscreenWidgets.enabled = *v;
    }
    if (auto schemaVersion = (*lockscreenWidgetsTbl)["schema_version"].value<int64_t>()) {
      lockscreenWidgets.schemaVersion = static_cast<std::int32_t>(*schemaVersion);
    }
    parseWidgetsPlacementSection(
        *lockscreenWidgetsTbl, lockscreenWidgets.grid, lockscreenWidgets.widgets, "lockscreen_widgets"
    );
  }

  // Parse [plugin_settings."author/plugin"] — open-ended per-plugin setting maps,
  // validated against the manifest schema (not the static pluginsSchema). Keys may
  // contain '/', so this is a top-level table rather than nested under [plugins].
  if (auto* pluginSettingsTbl = tbl["plugin_settings"].as_table()) {
    for (const auto& [pluginId, pluginNode] : *pluginSettingsTbl) {
      const auto* perPlugin = pluginNode.as_table();
      if (perPlugin == nullptr) {
        continue;
      }
      auto& bucket = config.plugins.pluginSettings[std::string(pluginId.str())];
      for (const auto& [key, value] : *perPlugin) {
        if (auto parsed = noctalia::config::readWidgetSettingValue(value); parsed.has_value()) {
          bucket[std::string(key.str())] = std::move(*parsed);
        }
      }
    }
  }

  if (config.bars.empty()) {
    if (logSummary) {
      kLog.info("no [bar.*] defined, using defaults");
    }
    config.bars.push_back(BarConfig{});
  }

  if (logSummary) {
    std::string barOrder;
    for (const auto& bar : config.bars) {
      if (!barOrder.empty()) {
        barOrder += ", ";
      }
      barOrder += bar.name;
    }
    kLog.info("{} bar(s) defined", config.bars.size());
    kLog.info("bar order: {}", barOrder);
    kLog.info("idle behaviors={}", config.idle.behaviors.size());
    std::size_t hookKindsUsed = 0;
    for (const auto& cmds : config.hooks.commands) {
      if (!cmds.empty()) {
        ++hookKindsUsed;
      }
    }
    kLog.info("hooks kinds with commands={}", hookKindsUsed);
  }

  if (logSchemaDiagnostics) {
    for (const auto& entry : schemaDiag.entries) {
      kLog.warn("{}: {}", entry.path, entry.message);
    }
  }
}

bool ConfigService::matchesKeybind(KeybindAction action, std::uint32_t sym, std::uint32_t modifiers) const {
  const auto& configured = keybindSet(m_config.keybinds, action);
  const auto active = configured.empty() ? defaultKeybindSet(action) : configured;
  return std::ranges::any_of(active, [sym, modifiers](const KeyChord& chord) {
    return keyChordMatches(chord, sym, modifiers);
  });
}

void ConfigService::registerIpc(IpcService& ipc) {
  ipc.registerHandler(
      "config-reload",
      [this](const std::string&) -> std::string {
        forceReload();
        return "ok\n";
      },
      "config-reload", "Reload the config file"
  );
}
