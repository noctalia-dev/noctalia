#include "config/config_service.h"

#include "compositors/compositor_detect.h"
#include "config/atomic_file.h"
#include "config/config_export.h"
#include "config/config_merge.h"
#include "config/schema/config_schema.h"
#include "config/schema/engine.h"
#include "config/widget_config.h"
#include "core/build_info.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/scoped_timer.h"
#include "ipc/ipc_service.h"
#include "notification/notification_manager.h"
#include "shell/desktop/desktop_widget_settings_registry.h"
#include "shell/settings/widget_settings_registry.h"
#include "system/distro_info.h"
#include "system/hardware_info.h"
#include "util/file_utils.h"
#include "util/string_utils.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
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
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace schema = noctalia::config::schema;

namespace {

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
        auto widget = readDesktopWidgetState(idNode.str(), *widgetTable, colorSection);
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

  constexpr Logger kLog("config");

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
        auto table = toml::parse_file(std::string(settingsPath));
        ConfigService::deepMerge(merged, table);
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
          m_notificationManager->addInternal("Noctalia", "Config parse error", pendingError, Urgency::Critical, 0);
    });
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
  return formatToml(*merged) + "\n";
}

std::string ConfigService::buildEffectiveConfigFromSources(
    std::string_view configDir, std::string_view settingsPath, std::string* error
) {
  const auto merged = mergeUserConfigSources(configDir, settingsPath, error);
  if (!merged.has_value()) {
    return {};
  }

  Config config;
  noctalia::config::seedBuiltinWidgets(config);
  if (merged->empty()) {
    config = makeDefaultConfig();
  } else {
    try {
      parseConfigTable(*merged, config, false, false);
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
    return;
  }

  kLog.info("loading {}", m_overridesPath);
  try {
    m_overridesTable = toml::parse_file(m_overridesPath);
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
        m_notificationManager->addInternal("Noctalia", "Config parse error", parseError, Urgency::Critical, 0);
  } else {
    m_pendingError = std::move(parseError);
  }
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

  // Apply the app-writable overrides overlay last — sidecar wins.
  deepMerge(merged, m_overridesTable);

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
    setConfigParseError(m_overridesParseError);
    refreshIncludeWatches();
    return;
  }

  std::string semanticError;
  try {
    parseConfigTable(merged, nextConfig, true);
  } catch (const std::exception& e) {
    semanticError = e.what();
    kLog.warn("config parse error: {}", semanticError);
  }

  if (semanticError.empty()) {
    m_lastChange = computeConfigChangeSet(m_config, nextConfig);
    m_config = std::move(nextConfig);
    m_configFileBarNames = std::move(configFileBarNames);
    m_configFileMonitorOverrideNames = std::move(configFileMonitorOverrideNames);
    m_configFileCalendarAccountNames = std::move(configFileCalendarAccountNames);
    extractWallpaperFromTable(merged);
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
                                                     : semanticError;
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
      schema::readInto(*barTbl, bar, schema::barFieldsSchema(), "bar." + bar.name, schemaDiag);

      // Parse [bar.<name>.monitor.*] overrides — insertion order preserved by toml++.
      if (auto* monTblMap = (*barTbl)["monitor"].as_table()) {
        for (const auto& [monName, monNode] : *monTblMap) {
          auto* monTbl = monNode.as_table();
          if (monTbl == nullptr) {
            continue;
          }
          BarMonitorOverride ovr;
          ovr.match = std::string(monName.str()); // key is the match unless an explicit `match` overrides it
          schema::readInto(
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

      validateWidgetSettings(widgetName, wc);
      config.widgets[widgetName] = std::move(wc);
    }
  }

  // Parse [shell]
  bool sessionActionsConfigured = false;
  if (auto* shellTbl = tbl["shell"].as_table()) {
    // Schema reads can't tell whether an empty actions list was explicit.
    sessionActionsConfigured = [&] {
      const auto* sessionTbl = (*shellTbl)["session"].as_table();
      return sessionTbl != nullptr && (*sessionTbl)["actions"].as_array() != nullptr;
    }();
    schema::readInto(*shellTbl, config.shell, schema::shellSchema(), "shell", schemaDiag);
  }
  if (!sessionActionsConfigured && config.shell.session.actions.empty()) {
    config.shell.session.actions = defaultSessionPanelActions();
  }

  // Parse [theme]
  if (auto* themeTbl = tbl["theme"].as_table()) {
    schema::readInto(*themeTbl, config.theme, schema::themeSchema(), "theme", schemaDiag);
  }

  // Parse [wallpaper] (config keys only; app-managed state keys default/last/
  // monitors/favorite are handled separately by extractWallpaperFromTable).
  if (auto* wpTbl = tbl["wallpaper"].as_table()) {
    schema::readInto(*wpTbl, config.wallpaper, schema::wallpaperSchema(), "wallpaper", schemaDiag);
  }

  // Parse [backdrop]
  if (auto* ovTbl = tbl["backdrop"].as_table()) {
    schema::readInto(*ovTbl, config.backdrop, schema::backdropSchema(), "backdrop", schemaDiag);
  }

  // Parse [lockscreen]
  if (auto* lockTbl = tbl["lockscreen"].as_table()) {
    schema::readInto(*lockTbl, config.lockscreen, schema::lockscreenSchema(), "lockscreen", schemaDiag);
  }

  // Parse [osd]
  if (auto* osdTbl = tbl["osd"].as_table()) {
    schema::readInto(*osdTbl, config.osd, schema::osdSchema(), "osd", schemaDiag);
  }

  if (auto* notifTbl = tbl["notification"].as_table()) {
    schema::readInto(*notifTbl, config.notification, schema::notificationSchema(), "notification", schemaDiag);
  }
  // Compatibility alias: accept [notifications] as well.
  if (auto* notifTbl = tbl["notifications"].as_table()) {
    schema::readInto(*notifTbl, config.notification, schema::notificationSchema(), "notifications", schemaDiag);
  }

  // Parse [dock]
  if (auto* dockTbl = tbl["dock"].as_table()) {
    schema::readInto(*dockTbl, config.dock, schema::dockSchema(), "dock", schemaDiag);
  }

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

  // Parse [hot_corners]
  if (auto* hotCornersTbl = tbl["hot_corners"].as_table()) {
    schema::readInto(*hotCornersTbl, config.hotCorners, schema::hotCornersSchema(), "hot_corners", schemaDiag);
  }

  // Parse [weather]
  if (auto* weatherTbl = tbl["weather"].as_table()) {
    schema::readInto(*weatherTbl, config.weather, schema::weatherSchema(), "weather", schemaDiag);
  }

  // Parse [calendar]
  if (auto* calendarTbl = tbl["calendar"].as_table()) {
    schema::readInto(*calendarTbl, config.calendar, schema::calendarSchema(), "calendar", schemaDiag);
  }

  // Parse [system]
  if (auto* systemTbl = tbl["system"].as_table()) {
    schema::readInto(*systemTbl, config.system, schema::systemSchema(), "system", schemaDiag);
  }

  // Parse [audio]
  if (auto* audioTbl = tbl["audio"].as_table()) {
    schema::readInto(*audioTbl, config.audio, schema::audioSchema(), "audio", schemaDiag);
  }

  // Parse [brightness]
  if (auto* brightnessTbl = tbl["brightness"].as_table()) {
    schema::readInto(*brightnessTbl, config.brightness, schema::brightnessSchema(), "brightness", schemaDiag);
  }

  // Parse [battery]
  if (auto* batteryTbl = tbl["battery"].as_table()) {
    schema::readInto(*batteryTbl, config.battery, schema::batterySchema(), "battery", schemaDiag);
  }

  // Parse [keybinds]
  if (auto* keybindsTbl = tbl["keybinds"].as_table()) {
    schema::readInto(*keybindsTbl, config.keybinds, schema::keybindsSchema(), "keybinds", schemaDiag);
  }

  // Parse [nightlight]
  if (auto* nightlightTbl = tbl["nightlight"].as_table()) {
    schema::readInto(*nightlightTbl, config.nightlight, schema::nightlightSchema(), "nightlight", schemaDiag);
  }

  // Parse [location]
  if (auto* locationTbl = tbl["location"].as_table()) {
    schema::readInto(*locationTbl, config.location, schema::locationSchema(), "location", schemaDiag);
  }

  // Parse [hooks]
  if (auto* hooksTbl = tbl["hooks"].as_table()) {
    schema::readInto(*hooksTbl, config.hooks, schema::hooksSchema(), "hooks", schemaDiag);
  }

  // Parse [control_center]. The default-shortcuts seeding stays here because it
  // must apply even when [control_center] (or its shortcuts array) is absent.
  bool controlCenterShortcutsConfigured = false;
  if (auto* ccTbl = tbl["control_center"].as_table()) {
    controlCenterShortcutsConfigured = (*ccTbl)["shortcuts"].as_array() != nullptr;
    schema::readInto(*ccTbl, config.controlCenter, schema::controlCenterSchema(), "control_center", schemaDiag);
  }
  if (!controlCenterShortcutsConfigured && config.controlCenter.shortcuts.empty()) {
    config.controlCenter.shortcuts = defaultControlCenterShortcuts();
  }

  // Parse [plugins]. Default-seeding stays here because it must apply even when
  // [plugins] (or its source array) is absent.
  bool pluginSourcesConfigured = false;
  if (auto* pluginsTbl = tbl["plugins"].as_table()) {
    pluginSourcesConfigured = (*pluginsTbl)["source"].as_array() != nullptr;
    schema::readInto(*pluginsTbl, config.plugins, schema::pluginsSchema(), "plugins", schemaDiag);
  }
  if (!pluginSourcesConfigured && config.plugins.sources.empty()) {
    config.plugins.sources = defaultPluginSources();
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

  // Parse [idle] and [idle.behavior.*]. Default-seeding stays here because it
  // must apply even when [idle] is absent.
  if (auto* idleTbl = tbl["idle"].as_table()) {
    schema::readInto(*idleTbl, config.idle, schema::idleSchema(), "idle", schemaDiag);
  }
  if (config.idle.behaviors.empty()) {
    config.idle.behaviors = defaultIdleBehaviors();
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
