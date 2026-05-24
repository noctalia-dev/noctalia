#include "config/config_service.h"

#include "config/config_export.h"
#include "core/build_info.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "ipc/ipc_service.h"
#include "notification/notification_manager.h"
#include "render/core/renderer.h"
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

namespace {

  std::optional<double> finiteDouble(const toml::node_view<const toml::node>& node) {
    if (auto v = node.value<double>()) {
      if (!std::isfinite(*v)) {
        return std::nullopt;
      }
      return *v;
    }
    if (auto v = node.value<int64_t>()) {
      return static_cast<double>(*v);
    }
    return std::nullopt;
  }

  std::string expandUserPathString(const std::string& path) {
    if (path.empty()) {
      return path;
    }
    return FileUtils::expandUserPath(path).string();
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

  std::vector<std::string> readStringOrArray(const toml::node& node) {
    if (auto* str = node.as_string()) {
      return {str->get()};
    }
    return readStringArray(node);
  }

  std::vector<ThemeConfig::TemplateCompareColorConfig> readTemplateCompareColors(const toml::node& node) {
    std::vector<ThemeConfig::TemplateCompareColorConfig> result;
    const auto* arr = node.as_array();
    if (arr == nullptr) {
      return result;
    }
    result.reserve(arr->size());
    for (const auto& item : *arr) {
      const auto* tbl = item.as_table();
      if (tbl == nullptr) {
        continue;
      }
      auto name = tbl->get_as<std::string>("name");
      auto color = tbl->get_as<std::string>("color");
      if (name != nullptr && color != nullptr) {
        result.push_back(ThemeConfig::TemplateCompareColorConfig{.name = name->get(), .color = color->get()});
      }
    }
    return result;
  }

  std::optional<WidgetSettingValue> readWidgetSetting(const toml::node& node) {
    if (const auto* stringValue = node.as_string()) {
      return WidgetSettingValue{stringValue->get()};
    }
    if (const auto* intValue = node.as_integer()) {
      return WidgetSettingValue{intValue->get()};
    }
    if (const auto* floatValue = node.as_floating_point()) {
      return WidgetSettingValue{floatValue->get()};
    }
    if (const auto* boolValue = node.as_boolean()) {
      return WidgetSettingValue{boolValue->get()};
    }
    if (const auto* arrayValue = node.as_array()) {
      std::vector<std::string> strings;
      for (const auto& item : *arrayValue) {
        if (auto value = item.value<std::string>()) {
          strings.push_back(*value);
        }
      }
      return WidgetSettingValue{std::move(strings)};
    }
    return std::nullopt;
  }

  [[nodiscard]] bool isCommonWidgetColorSetting(std::string_view key) {
    return key == "color" || key == "capsule_fill" || key == "capsule_border" || key == "capsule_foreground";
  }

  [[nodiscard]] bool isWidgetColorSetting(std::string_view type, std::string_view key) {
    if (isCommonWidgetColorSetting(key)) {
      return true;
    }
    if (type == "audio_visualizer") {
      return key == "low_color" || key == "high_color";
    }
    if (type == "battery") {
      return key == "warning_color";
    }
    if (type == "workspaces") {
      return key == "focused_color" || key == "occupied_color" || key == "empty_color";
    }
    return false;
  }

  [[nodiscard]] bool isDesktopWidgetColorSetting(std::string_view type, std::string_view key) {
    if (key == "background_color") {
      return true;
    }
    if (type == "clock" || type == "weather" || type == "media_player") {
      return key == "color";
    }
    if (type == "audio_visualizer") {
      return key == "low_color" || key == "high_color";
    }
    if (type == "sysmon") {
      return key == "color" || key == "color2";
    }
    return false;
  }

  [[nodiscard]] std::optional<std::string>
  colorStringValue(const toml::table& table, std::string_view key, const std::string& context) {
    if (!table.contains(key)) {
      return std::nullopt;
    }
    if (auto value = table[key].value<std::string>()) {
      return *value;
    }
    throw std::runtime_error(context + ": expected string ColorSpec");
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
    for (const auto& [key, value] : widget.settings) {
      if (!isWidgetColorSetting(widget.type, key)) {
        continue;
      }
      const bool allowEmpty = key == "capsule_border";
      validateWidgetColorSettingValue(value, "widget." + std::string(widgetName) + "." + key, allowEmpty);
    }
  }

  void validateDesktopWidgetColorSettings(const DesktopWidgetState& widget) {
    for (const auto& [key, value] : widget.settings) {
      if (!isDesktopWidgetColorSetting(widget.type, key)) {
        continue;
      }
      validateWidgetColorSettingValue(value, "desktop_widgets.widget." + widget.id + ".settings." + key);
    }
  }

  DesktopWidgetState readDesktopWidgetState(std::string_view id, const toml::table& widgetTable) {
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
    if (auto scale = finiteDouble(widgetTable["scale"])) {
      widget.scale = std::clamp(static_cast<float>(*scale), 0.2f, 8.0f);
    }
    if (auto rotation = finiteDouble(widgetTable["rotation"])) {
      widget.rotationRad = static_cast<float>(*rotation);
    }
    if (auto enabled = widgetTable["enabled"].value<bool>()) {
      widget.enabled = *enabled;
    }
    if (const auto* settingsTable = widgetTable["settings"].as_table()) {
      for (const auto& [key, value] : *settingsTable) {
        if (auto parsed = readWidgetSetting(value); parsed.has_value()) {
          widget.settings.emplace(std::string(key.str()), std::move(*parsed));
        }
      }
    }
    validateDesktopWidgetColorSettings(widget);
    return widget;
  }

  void setHookCommandsFromNode(const toml::node& node, std::vector<std::string>& out) {
    out.clear();
    if (auto* s = node.as_string()) {
      const auto& val = s->get();
      if (!val.empty()) {
        out.push_back(val);
      }
      return;
    }
    for (const auto& line : readStringArray(node)) {
      if (!line.empty()) {
        out.push_back(line);
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
    std::sort(files.begin(), files.end());
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

  std::optional<ColorSpec> optionalCapsuleBorder(const std::string& raw, std::string_view context) {
    if (StringUtils::trim(raw).empty()) {
      return std::nullopt;
    }
    return colorSpecFromConfigString(raw, context);
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
    m_setupMarkerPath = dir + "/.setup-complete";
  }

  loadOverridesFromFile();
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
    ::close(m_inotifyFd);
  }
}

// ── Public interface ─────────────────────────────────────────────────────────

void ConfigService::addReloadCallback(ReloadCallback callback) { m_reloadCallbacks.push_back(std::move(callback)); }

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
      (oldDefault != m_defaultWallpaperPath || oldLast != m_lastWallpaperPath ||
       oldMonitors != m_monitorWallpaperPaths);
  if (wallpaperChanged && m_wallpaperChangeCallback) {
    m_wallpaperChangeCallback();
  }
  fireReloadCallbacks();
}

void ConfigService::fireReloadCallbacks() {
  for (const auto& cb : m_reloadCallbacks) {
    cb();
  }
}

bool ConfigService::shouldRunSetupWizard() const {
  // Single canonical signal: the marker file. If we have no state dir we cannot
  // persist completion, so never show the wizard (it would loop forever).
  return !m_setupMarkerPath.empty() && !std::filesystem::exists(m_setupMarkerPath);
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

  toml::table paths;
  paths.insert_or_assign("config_dir", m_configDir);
  paths.insert_or_assign("settings_path", m_overridesPath);
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
  toml::table merged;

  for (const auto& path : sortedConfigTomlFiles(m_configDir)) {
    try {
      auto table = toml::parse_file(path.string());
      deepMerge(merged, table);
    } catch (const toml::parse_error& e) {
      kLog.warn("skipping parse error in merged user config export {}: {}", path.filename().string(), e.description());
    }
  }

  if (!m_overridesPath.empty() && std::filesystem::exists(m_overridesPath)) {
    try {
      auto table = toml::parse_file(m_overridesPath);
      deepMerge(merged, table);
    } catch (const toml::parse_error& e) {
      kLog.warn("skipping parse error in merged user config export {}: {}", m_overridesPath, e.description());
    }
  }

  return formatToml(merged) + "\n";
}

std::string ConfigService::buildEffectiveConfig() const {
  return formatToml(config_export::configToToml(m_config)) + "\n";
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
        } else if (event->wd == m_overridesWatchWd) {
          const auto overridesFilename = std::filesystem::path(m_overridesPath).filename().string();
          if (name == overridesFilename) {
            overridesChanged = true;
          }
        } else {
          // Check whether this event comes from a symlink-target directory.
          const auto symIt = m_symlinkDirWds.find(event->wd);
          if (symIt != m_symlinkDirWds.end()) {
            for (const auto& watched : symIt->second) {
              if (name == watched) {
                configChanged = true;
                break;
              }
            }
          }
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
      (oldDefault != m_defaultWallpaperPath || oldLast != m_lastWallpaperPath ||
       oldMonitors != m_monitorWallpaperPaths);
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

    if (ovr.enabled)
      resolved.enabled = *ovr.enabled;
    if (ovr.autoHide)
      resolved.autoHide = *ovr.autoHide;
    if (ovr.reserveSpace)
      resolved.reserveSpace = *ovr.reserveSpace;
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
    if (ovr.padding)
      resolved.padding = *ovr.padding;
    if (ovr.widgetSpacing)
      resolved.widgetSpacing = *ovr.widgetSpacing;
    if (ovr.shadow)
      resolved.shadow = *ovr.shadow;
    if (ovr.contactShadow)
      resolved.contactShadow = *ovr.contactShadow;
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
        m_symlinkDirWds[wd].push_back(realName);
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

void ConfigService::seedBuiltinWidgets(Config& config) {
  // Built-in named widget instances — act as defaults that [widget.*] entries override.
  auto seed = [&](const char* name, WidgetConfig wc) { config.widgets.emplace(name, std::move(wc)); };

  WidgetConfig cpu;
  cpu.type = "sysmon";
  cpu.settings["stat"] = std::string("cpu_usage");
  seed("cpu", std::move(cpu));

  WidgetConfig temp;
  temp.type = "sysmon";
  temp.settings["stat"] = std::string("cpu_temp");
  seed("temp", std::move(temp));

  WidgetConfig ram;
  ram.type = "sysmon";
  ram.settings["stat"] = std::string("ram_used");
  seed("ram", std::move(ram));

  WidgetConfig netTx;
  netTx.type = "sysmon";
  netTx.settings["stat"] = std::string("net_tx");
  seed("network_tx", std::move(netTx));

  WidgetConfig netRx;
  netRx.type = "sysmon";
  netRx.settings["stat"] = std::string("net_rx");
  seed("network_rx", std::move(netRx));

  WidgetConfig outputVolume;
  outputVolume.type = "volume";
  outputVolume.settings["device"] = std::string("output");
  seed("output_volume", std::move(outputVolume));

  WidgetConfig inputVolume;
  inputVolume.type = "volume";
  inputVolume.settings["device"] = std::string("input");
  seed("input_volume", std::move(inputVolume));

  WidgetConfig date;
  date.type = "clock";
  date.settings["format"] = std::string("{:%a %d %b}");
  seed("date", std::move(date));

  WidgetConfig activeWindow;
  activeWindow.type = "active_window";
  activeWindow.settings["max_length"] = 260.0;
  activeWindow.settings["min_length"] = 80.0;
  activeWindow.settings["icon_size"] = static_cast<double>(Style::fontSizeBody);
  activeWindow.settings["title_scroll"] = std::string("none");
  seed("active_window", std::move(activeWindow));

  WidgetConfig media;
  media.type = "media";
  media.settings["max_length"] = 220.0;
  media.settings["min_length"] = 80.0;
  media.settings["art_size"] = 16.0;
  media.settings["title_scroll"] = std::string("none");
  seed("media", std::move(media));

  WidgetConfig keyboardLayout;
  keyboardLayout.type = "keyboard_layout";
  keyboardLayout.settings["cycle_command"] = std::string("");
  seed("keyboard_layout", std::move(keyboardLayout));

  WidgetConfig lockKeys;
  lockKeys.type = "lock_keys";
  lockKeys.settings["show_caps_lock"] = true;
  lockKeys.settings["show_num_lock"] = true;
  lockKeys.settings["show_scroll_lock"] = false;
  lockKeys.settings["hide_when_off"] = false;
  lockKeys.settings["display"] = std::string("short");
  seed("lock_keys", std::move(lockKeys));

  WidgetConfig spacer;
  spacer.type = "spacer";
  seed("spacer", std::move(spacer));
}

void ConfigService::loadAll() {
  m_effectiveOverrideCache.clear();
  auto makeDefaultConfig = [] {
    Config config;
    ConfigService::seedBuiltinWidgets(config);
    config.idle.behaviors = defaultIdleBehaviors();
    config.bars.push_back(BarConfig{});
    config.controlCenter.shortcuts = defaultControlCenterShortcuts();
    config.shell.session.actions = defaultSessionPanelActions();
    return config;
  };

  Config nextConfig;
  seedBuiltinWidgets(nextConfig);

  const auto files = sortedConfigTomlFiles(m_configDir);

  toml::table merged;
  std::string firstError;

  for (const auto& path : files) {
    try {
      auto tbl = toml::parse_file(path.string());
      deepMerge(merged, tbl);
      kLog.info("loaded {}", path.string());
    } catch (const toml::parse_error& e) {
      const auto& src = e.source();
      kLog.warn(
          "parse error in {} at line {}, column {}: {}", path.filename().string(), src.begin.line, src.begin.column,
          e.description()
      );
      if (firstError.empty()) {
        firstError = std::format(
            "{} line {}, column {}: {}", path.filename().string(), src.begin.line, src.begin.column, e.description()
        );
      }
    }
  }

  decltype(m_configFileBarNames) configFileBarNames;
  decltype(m_configFileMonitorOverrideNames) configFileMonitorOverrideNames;
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

  // Apply the app-writable overrides overlay last — sidecar wins.
  deepMerge(merged, m_overridesTable);

  if (files.empty() && m_overridesTable.empty()) {
    kLog.info("no config files found, using defaults");
    m_config = makeDefaultConfig();
    m_configFileBarNames.clear();
    m_configFileMonitorOverrideNames.clear();
    m_defaultWallpaperPath.clear();
    m_lastWallpaperPath.clear();
    m_monitorWallpaperPaths.clear();
    setConfigParseError(m_overridesParseError);
    return;
  }

  std::string semanticError;
  try {
    parseTableInto(merged, nextConfig, true);
  } catch (const std::exception& e) {
    semanticError = e.what();
    kLog.warn("config parse error: {}", semanticError);
  }

  if (semanticError.empty()) {
    m_config = std::move(nextConfig);
    m_configFileBarNames = std::move(configFileBarNames);
    m_configFileMonitorOverrideNames = std::move(configFileMonitorOverrideNames);
    extractWallpaperFromTable(merged);
  } else if (m_config.bars.empty()) {
    m_config = makeDefaultConfig();
    m_configFileBarNames.clear();
    m_configFileMonitorOverrideNames.clear();
    m_defaultWallpaperPath.clear();
    m_lastWallpaperPath.clear();
    m_monitorWallpaperPaths.clear();
  }

  const std::string parseError = !firstError.empty()              ? firstError
                                 : !m_overridesParseError.empty() ? m_overridesParseError
                                                                  : semanticError;
  setConfigParseError(parseError);
}

void ConfigService::parseTableInto(const toml::table& tbl, Config& config, bool logSummary) const {
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
      if (auto v = (*barTbl)["position"].value<std::string>())
        bar.position = *v;
      if (auto v = (*barTbl)["enabled"].value<bool>())
        bar.enabled = *v;
      if (auto v = (*barTbl)["auto_hide"].value<bool>())
        bar.autoHide = *v;
      if (auto v = (*barTbl)["reserve_space"].value<bool>())
        bar.reserveSpace = *v;
      if (auto v = (*barTbl)["thickness"].value<int64_t>())
        bar.thickness = std::clamp(static_cast<std::int32_t>(*v), 10, 300);
      if (auto v = finiteDouble((*barTbl)["background_opacity"]))
        bar.backgroundOpacity = std::clamp(static_cast<float>(*v), 0.0f, 1.0f);
      if (auto borderStr = colorStringValue(*barTbl, "border", "bar." + bar.name + ".border"))
        bar.border = colorSpecFromConfigString(*borderStr, "bar." + bar.name + ".border");
      if (auto v = finiteDouble((*barTbl)["border_width"]))
        bar.borderWidth = std::clamp(static_cast<float>(*v), 0.0f, 20.0f);
      if (auto v = (*barTbl)["radius"].value<int64_t>()) {
        const auto r = std::clamp(static_cast<std::int32_t>(*v), 0, 500);
        bar.radius = r;
        bar.radiusTopLeft = r;
        bar.radiusTopRight = r;
        bar.radiusBottomLeft = r;
        bar.radiusBottomRight = r;
      }
      if (auto v = (*barTbl)["radius_top_left"].value<int64_t>())
        bar.radiusTopLeft = std::clamp(static_cast<std::int32_t>(*v), 0, 500);
      if (auto v = (*barTbl)["radius_top_right"].value<int64_t>())
        bar.radiusTopRight = std::clamp(static_cast<std::int32_t>(*v), 0, 500);
      if (auto v = (*barTbl)["radius_bottom_left"].value<int64_t>())
        bar.radiusBottomLeft = std::clamp(static_cast<std::int32_t>(*v), 0, 500);
      if (auto v = (*barTbl)["radius_bottom_right"].value<int64_t>())
        bar.radiusBottomRight = std::clamp(static_cast<std::int32_t>(*v), 0, 500);
      if (auto v = (*barTbl)["margin_ends"].value<int64_t>())
        bar.marginEnds = static_cast<std::int32_t>(*v);
      if (auto v = (*barTbl)["margin_edge"].value<int64_t>())
        bar.marginEdge = static_cast<std::int32_t>(*v);
      if (auto v = (*barTbl)["padding"].value<int64_t>())
        bar.padding = static_cast<std::int32_t>(*v);
      if (auto v = (*barTbl)["widget_spacing"].value<int64_t>())
        bar.widgetSpacing = static_cast<std::int32_t>(*v);
      if (auto v = (*barTbl)["shadow"].value<bool>())
        bar.shadow = *v;
      if (auto v = (*barTbl)["contact_shadow"].value<bool>())
        bar.contactShadow = *v;
      if (auto v = finiteDouble((*barTbl)["scale"]))
        bar.scale = std::clamp(static_cast<float>(*v), 0.5f, 4.0f);
      if (auto fontWeightValue = (*barTbl)["font_weight"].value<int64_t>()) {
        bar.fontWeight = static_cast<int>(*fontWeightValue);
      }
      if (auto* n = (*barTbl)["start"].as_array())
        bar.startWidgets = readStringArray(*n);
      if (auto* n = (*barTbl)["center"].as_array())
        bar.centerWidgets = readStringArray(*n);
      if (auto* n = (*barTbl)["end"].as_array())
        bar.endWidgets = readStringArray(*n);

      if (auto v = (*barTbl)["capsule"].value<bool>()) {
        bar.widgetCapsuleDefault = *v;
      }
      if (auto fillStr = colorStringValue(*barTbl, "capsule_fill", "bar." + bar.name + ".capsule_fill")) {
        bar.widgetCapsuleFill = colorSpecFromConfigString(*fillStr, "bar." + bar.name + ".capsule_fill");
      }
      if (auto fgStr = colorStringValue(*barTbl, "capsule_foreground", "bar." + bar.name + ".capsule_foreground")) {
        bar.widgetCapsuleForeground = colorSpecFromConfigString(*fgStr, "bar." + bar.name + ".capsule_foreground");
      }
      if (auto v = finiteDouble((*barTbl)["capsule_padding"])) {
        bar.widgetCapsulePadding = std::clamp(static_cast<float>(*v), 0.0f, 48.0f);
      }
      if (auto v = finiteDouble((*barTbl)["capsule_radius"])) {
        bar.widgetCapsuleRadius = std::clamp(*v, 0.0, 80.0);
      }
      if (auto v = finiteDouble((*barTbl)["capsule_opacity"])) {
        bar.widgetCapsuleOpacity = std::clamp(static_cast<float>(*v), 0.0f, 1.0f);
      }
      if (barTbl->contains("capsule_border")) {
        bar.widgetCapsuleBorderSpecified = true;
        const std::string context = "bar." + bar.name + ".capsule_border";
        bar.widgetCapsuleBorder = optionalCapsuleBorder(*colorStringValue(*barTbl, "capsule_border", context), context);
      }
      if (auto widgetColorStr = colorStringValue(*barTbl, "color", "bar." + bar.name + ".color")) {
        bar.widgetColor = colorSpecFromConfigString(*widgetColorStr, "bar." + bar.name + ".color");
      }
      if (auto* n = (*barTbl)["capsule_groups"].as_array()) {
        bar.widgetCapsuleGroups = readStringArray(*n);
      }

      // Parse [bar.<name>.monitor.*] overrides — insertion order preserved by toml++
      if (auto* monTblMap = (*barTbl)["monitor"].as_table()) {
        for (const auto& [monName, monNode] : *monTblMap) {
          auto* monTbl = monNode.as_table();
          if (monTbl == nullptr) {
            continue;
          }

          BarMonitorOverride ovr;
          if (auto v = (*monTbl)["match"].value<std::string>()) {
            ovr.match = *v;
          } else {
            ovr.match = std::string(monName.str()); // key is the match if not explicit
          }

          if (auto v = (*monTbl)["enabled"].value<bool>())
            ovr.enabled = *v;
          if (auto v = (*monTbl)["auto_hide"].value<bool>())
            ovr.autoHide = *v;
          if (auto v = (*monTbl)["reserve_space"].value<bool>())
            ovr.reserveSpace = *v;
          if (auto v = (*monTbl)["thickness"].value<int64_t>())
            ovr.thickness = std::clamp(static_cast<std::int32_t>(*v), 10, 300);
          if (auto v = finiteDouble((*monTbl)["background_opacity"]))
            ovr.backgroundOpacity = std::clamp(static_cast<float>(*v), 0.0f, 1.0f);
          const std::string monitorContext = "bar." + bar.name + ".monitor." + std::string(monName.str());
          if (auto borderStr = colorStringValue(*monTbl, "border", monitorContext + ".border"))
            ovr.border = colorSpecFromConfigString(*borderStr, monitorContext + ".border");
          if (auto v = finiteDouble((*monTbl)["border_width"]))
            ovr.borderWidth = std::clamp(static_cast<float>(*v), 0.0f, 20.0f);
          if (auto v = (*monTbl)["radius"].value<int64_t>())
            ovr.radius = std::clamp(static_cast<std::int32_t>(*v), 0, 500);
          if (auto v = (*monTbl)["radius_top_left"].value<int64_t>())
            ovr.radiusTopLeft = std::clamp(static_cast<std::int32_t>(*v), 0, 500);
          if (auto v = (*monTbl)["radius_top_right"].value<int64_t>())
            ovr.radiusTopRight = std::clamp(static_cast<std::int32_t>(*v), 0, 500);
          if (auto v = (*monTbl)["radius_bottom_left"].value<int64_t>())
            ovr.radiusBottomLeft = std::clamp(static_cast<std::int32_t>(*v), 0, 500);
          if (auto v = (*monTbl)["radius_bottom_right"].value<int64_t>())
            ovr.radiusBottomRight = std::clamp(static_cast<std::int32_t>(*v), 0, 500);
          if (auto v = (*monTbl)["margin_ends"].value<int64_t>())
            ovr.marginEnds = static_cast<std::int32_t>(*v);
          if (auto v = (*monTbl)["margin_edge"].value<int64_t>())
            ovr.marginEdge = static_cast<std::int32_t>(*v);
          if (auto v = (*monTbl)["padding"].value<int64_t>())
            ovr.padding = static_cast<std::int32_t>(*v);
          if (auto v = (*monTbl)["widget_spacing"].value<int64_t>())
            ovr.widgetSpacing = static_cast<std::int32_t>(*v);
          if (auto v = finiteDouble((*monTbl)["scale"]))
            ovr.scale = std::clamp(static_cast<float>(*v), 0.5f, 4.0f);
          if (auto v = (*monTbl)["shadow"].value<bool>())
            ovr.shadow = *v;
          if (auto v = (*monTbl)["contact_shadow"].value<bool>())
            ovr.contactShadow = *v;
          if (auto* n = (*monTbl)["start"].as_array())
            ovr.startWidgets = readStringArray(*n);
          if (auto* n = (*monTbl)["center"].as_array())
            ovr.centerWidgets = readStringArray(*n);
          if (auto* n = (*monTbl)["end"].as_array())
            ovr.endWidgets = readStringArray(*n);

          if (auto v = (*monTbl)["capsule"].value<bool>()) {
            ovr.widgetCapsuleDefault = *v;
          }
          if (auto fillStr = colorStringValue(*monTbl, "capsule_fill", monitorContext + ".capsule_fill")) {
            ovr.widgetCapsuleFill = colorSpecFromConfigString(*fillStr, monitorContext + ".capsule_fill");
          }
          if (auto fgStr = colorStringValue(*monTbl, "capsule_foreground", monitorContext + ".capsule_foreground")) {
            ovr.widgetCapsuleForeground = colorSpecFromConfigString(*fgStr, monitorContext + ".capsule_foreground");
          }
          if (auto v = finiteDouble((*monTbl)["capsule_padding"])) {
            ovr.widgetCapsulePadding = std::clamp(*v, 0.0, 48.0);
          }
          if (auto v = finiteDouble((*monTbl)["capsule_radius"])) {
            ovr.widgetCapsuleRadius = std::clamp(*v, 0.0, 80.0);
          }
          if (auto v = finiteDouble((*monTbl)["capsule_opacity"])) {
            ovr.widgetCapsuleOpacity = std::clamp(*v, 0.0, 1.0);
          }
          if (monTbl->contains("capsule_border")) {
            ovr.widgetCapsuleBorderSpecified = true;
            ovr.widgetCapsuleBorder = optionalCapsuleBorder(
                *colorStringValue(*monTbl, "capsule_border", monitorContext + ".capsule_border"),
                monitorContext + ".capsule_border"
            );
          }
          if (auto cStr = colorStringValue(*monTbl, "color", monitorContext + ".color")) {
            ovr.widgetColor = colorSpecFromConfigString(*cStr, monitorContext + ".color");
          }
          if (auto* n = (*monTbl)["capsule_groups"].as_array()) {
            ovr.widgetCapsuleGroups = readStringArray(*n);
          }

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

      std::string widgetName(name.str());
      WidgetConfig wc;

      if (auto v = (*entryTbl)["type"].value<std::string>()) {
        wc.type = *v;
        if (auto it = config.widgets.find(widgetName); it != config.widgets.end() && it->second.type == wc.type) {
          wc.settings = it->second.settings;
        }
      } else if (auto it = config.widgets.find(widgetName); it != config.widgets.end()) {
        wc = it->second;
      } else {
        wc.type = widgetName;
      }

      for (const auto& [key, val] : *entryTbl) {
        if (key == "type") {
          continue;
        }
        if (auto* s = val.as_string()) {
          wc.settings[std::string(key.str())] = s->get();
        } else if (auto* i = val.as_integer()) {
          wc.settings[std::string(key.str())] = i->get();
        } else if (auto* f = val.as_floating_point()) {
          wc.settings[std::string(key.str())] = f->get();
        } else if (auto* b = val.as_boolean()) {
          wc.settings[std::string(key.str())] = b->get();
        } else if (auto* arr = val.as_array()) {
          std::vector<std::string> list;
          list.reserve(arr->size());
          for (const auto& item : *arr) {
            if (auto v = item.value<std::string>()) {
              list.push_back(*v);
            }
          }
          wc.settings[std::string(key.str())] = std::move(list);
        }
      }

      validateWidgetColorSettings(widgetName, wc);
      config.widgets[widgetName] = std::move(wc);
    }
  }

  // Parse [shell]
  if (auto* shellTbl = tbl["shell"].as_table()) {
    auto& shell = config.shell;
    if (auto v = finiteDouble((*shellTbl)["ui_scale"])) {
      shell.uiScale = std::clamp(static_cast<float>(*v), 0.5f, 4.0f);
    }
    if (auto v = finiteDouble((*shellTbl)["corner_radius_scale"])) {
      shell.cornerRadiusScale = std::clamp(static_cast<float>(*v), 0.0f, 2.0f);
    }
    if (auto v = (*shellTbl)["font_family"].value<std::string>()) {
      shell.fontFamily = StringUtils::trim(*v);
      if (shell.fontFamily.empty()) {
        shell.fontFamily = "sans-serif";
      }
    }
    if (auto v = (*shellTbl)["lang"].value<std::string>()) {
      shell.lang = *v;
    }
    if (auto v = (*shellTbl)["time_format"].value<std::string>()) {
      shell.timeFormat = *v;
    }
    if (auto v = (*shellTbl)["date_format"].value<std::string>()) {
      shell.dateFormat = *v;
    }
    if (auto v = (*shellTbl)["offline_mode"].value<bool>()) {
      shell.offlineMode = *v;
    }
    if (auto v = (*shellTbl)["telemetry_enabled"].value<bool>()) {
      shell.telemetryEnabled = *v;
    }
    if (auto v = (*shellTbl)["niri_overview_type_to_launch_enabled"].value<bool>()) {
      shell.niriOverviewTypeToLaunchEnabled = *v;
    }
    if (auto polkitAgent = (*shellTbl)["polkit_agent"].value<bool>()) {
      shell.polkitAgent = *polkitAgent;
    }
    if (auto v = (*shellTbl)["password_style"].value<std::string>()) {
      if (auto parsed = enumFromKey(kPasswordMaskStyles, *v)) {
        shell.passwordMaskStyle = *parsed;
      }
    }
    if (const auto* animationTbl = (*shellTbl)["animation"].as_table()) {
      if (auto enabled = (*animationTbl)["enabled"].value<bool>()) {
        shell.animation.enabled = *enabled;
      }
      if (auto v = finiteDouble((*animationTbl)["speed"])) {
        shell.animation.speed = std::clamp(static_cast<float>(*v), 0.05f, 4.0f);
      }
    }
    if (const auto* shadowTbl = (*shellTbl)["shadow"].as_table()) {
      if (auto v = (*shadowTbl)["blur"].value<int64_t>()) {
        shell.shadow.blur = std::clamp(static_cast<std::int32_t>(*v), 0, 100);
      }
      if (auto v = (*shadowTbl)["offset_x"].value<int64_t>()) {
        shell.shadow.offsetX = std::clamp(static_cast<std::int32_t>(*v), -40, 40);
      }
      if (auto v = (*shadowTbl)["offset_y"].value<int64_t>()) {
        shell.shadow.offsetY = std::clamp(static_cast<std::int32_t>(*v), -40, 40);
      }
      if (auto v = finiteDouble((*shadowTbl)["alpha"])) {
        shell.shadow.alpha = std::clamp(static_cast<float>(*v), 0.0f, 1.0f);
      }
    }
    if (const auto* panelTbl = (*shellTbl)["panel"].as_table()) {
      if (auto v = (*panelTbl)["background_blur"].value<bool>()) {
        shell.panel.backgroundBlur = *v;
      }
      if (auto v = (*panelTbl)["borders"].value<bool>()) {
        shell.panel.borders = *v;
      }
      if (auto v = (*panelTbl)["shadow"].value<bool>()) {
        shell.panel.shadow = *v;
      }
      if (auto v = (*panelTbl)["transparency_mode"].value<std::string>()) {
        if (auto parsed = enumFromKey(kPanelTransparencyModes, StringUtils::trim(*v))) {
          shell.panel.transparencyMode = *parsed;
        }
      }
      if (auto v = (*panelTbl)["launcher_placement"].value<std::string>()) {
        if (auto parsed = enumFromKey(kPanelPlacements, StringUtils::trim(*v))) {
          shell.panel.launcherPlacement = *parsed;
        }
      }
      if (auto v = (*panelTbl)["clipboard_placement"].value<std::string>()) {
        if (auto parsed = enumFromKey(kPanelPlacements, StringUtils::trim(*v))) {
          shell.panel.clipboardPlacement = *parsed;
        }
      }
      if (auto v = (*panelTbl)["control_center_placement"].value<std::string>()) {
        if (auto parsed = enumFromKey(kPanelPlacements, StringUtils::trim(*v))) {
          shell.panel.controlCenterPlacement = *parsed;
        }
      }
      if (auto v = (*panelTbl)["wallpaper_placement"].value<std::string>()) {
        if (auto parsed = enumFromKey(kPanelPlacements, StringUtils::trim(*v))) {
          shell.panel.wallpaperPlacement = *parsed;
        }
      }
      if (auto v = (*panelTbl)["session_placement"].value<std::string>()) {
        if (auto parsed = enumFromKey(kPanelPlacements, StringUtils::trim(*v))) {
          shell.panel.sessionPlacement = *parsed;
        }
      }
      if (auto v = (*panelTbl)["open_near_click_control_center"].value<bool>()) {
        shell.panel.openNearClickControlCenter = *v;
      }
      if (auto v = (*panelTbl)["open_near_click_launcher"].value<bool>()) {
        shell.panel.openNearClickLauncher = *v;
      }
      if (auto v = (*panelTbl)["open_near_click_clipboard"].value<bool>()) {
        shell.panel.openNearClickClipboard = *v;
      }
      if (auto v = (*panelTbl)["open_near_click_wallpaper"].value<bool>()) {
        shell.panel.openNearClickWallpaper = *v;
      }
      if (auto v = (*panelTbl)["open_near_click_session"].value<bool>()) {
        shell.panel.openNearClickSession = *v;
      }
      if (auto v = (*panelTbl)["launcher_categories"].value<bool>()) {
        shell.panel.launcherCategories = *v;
      }
    }
    if (const auto* screenCornersTbl = (*shellTbl)["screen_corners"].as_table()) {
      if (auto v = (*screenCornersTbl)["enabled"].value<bool>()) {
        shell.screenCorners.enabled = *v;
      }
      if (auto v = (*screenCornersTbl)["size"].value<std::int64_t>()) {
        shell.screenCorners.size = std::clamp(static_cast<std::int32_t>(*v), 1, 100);
      }
    }
    if (const auto* mprisTbl = (*shellTbl)["mpris"].as_table()) {
      if (const auto* blacklistNode = mprisTbl->get("blacklist")) {
        shell.mpris.blacklist = readStringArray(*blacklistNode);
      }
    }
    if (auto v = (*shellTbl)["avatar_path"].value<std::string>()) {
      shell.avatarPath = *v;
    }
    if (auto v = (*shellTbl)["settings_show_advanced"].value<bool>()) {
      shell.settingsShowAdvanced = *v;
    }
    if (auto v = (*shellTbl)["middle_click_opens_widget_settings"].value<bool>()) {
      shell.middleClickOpensWidgetSettings = *v;
    }
    if (auto v = (*shellTbl)["show_location"].value<bool>()) {
      shell.showLocation = *v;
    }
    if (auto v = (*shellTbl)["launch_apps_as_systemd_services"].value<bool>()) {
      shell.launchAppsAsSystemdServices = *v;
    }
    if (auto v = (*shellTbl)["clipboard_enabled"].value<bool>()) {
      shell.clipboardEnabled = *v;
    }
    if (auto v = (*shellTbl)["clipboard_history_max_entries"].value<int64_t>()) {
      shell.clipboardHistoryMaxEntries = static_cast<int>(std::clamp<int64_t>(*v, 10, 200));
    }
    if (auto v = (*shellTbl)["screen_time_enabled"].value<bool>()) {
      shell.screenTimeEnabled = *v;
    }
    if (auto v = (*shellTbl)["clipboard_auto_paste"].value<std::string>()) {
      if (auto parsed = enumFromKey(kClipboardAutoPasteModes, *v)) {
        shell.clipboardAutoPaste = *parsed;
      }
    }
    if (auto v = (*shellTbl)["clipboard_image_action_command"].value<std::string>()) {
      shell.clipboardImageActionCommand = *v;
    }

    bool sessionActionsKeyPresent = false;
    if (const auto* sessionTbl = (*shellTbl)["session"].as_table()) {
      if (sessionTbl->contains("actions")) {
        sessionActionsKeyPresent = true;
        shell.session.actions.clear();
        if (const auto* actionsArr = (*sessionTbl)["actions"].as_array()) {
          for (const auto& entry : *actionsArr) {
            auto* entryTbl = entry.as_table();
            if (entryTbl == nullptr) {
              continue;
            }
            SessionPanelActionConfig row{};
            if (auto v = (*entryTbl)["action"].value<std::string>()) {
              row.action = StringUtils::toLower(StringUtils::trim(*v));
            }
            if (row.action.empty()) {
              continue;
            }
            if (auto v = (*entryTbl)["enabled"].value<bool>()) {
              row.enabled = *v;
            }
            if (const auto* cmdNode = entryTbl->get("command")) {
              if (auto s = cmdNode->value<std::string>()) {
                row.command = StringUtils::trim(*s);
                if (row.command->empty()) {
                  row.command = std::nullopt;
                }
              }
            }
            if (auto v = (*entryTbl)["label"].value<std::string>()) {
              row.label = StringUtils::trim(*v);
              if (row.label->empty()) {
                row.label = std::nullopt;
              }
            }
            if (auto v = (*entryTbl)["glyph"].value<std::string>()) {
              row.glyph = StringUtils::trim(*v);
              if (row.glyph->empty()) {
                row.glyph = std::nullopt;
              }
            }
            if (auto v = (*entryTbl)["variant"].value<std::string>()) {
              const std::string key = StringUtils::trim(*v);
              if (auto parsed = enumFromKey(kSessionActionButtonVariants, key)) {
                row.variant = *parsed;
              } else {
                kLog.warn("unknown shell.session.actions variant \"{}\"", key);
              }
            }
            if (auto v = (*entryTbl)["shortcut"].value<std::string>()) {
              const std::string s = StringUtils::trim(*v);
              if (!s.empty()) {
                row.shortcut = parseKeyChordSpec(s);
              }
            }
            shell.session.actions.push_back(std::move(row));
          }
        }
      }
    }
    if (!sessionActionsKeyPresent && shell.session.actions.empty()) {
      shell.session.actions = defaultSessionPanelActions();
    }
  }

  // Parse [theme]
  if (auto* themeTbl = tbl["theme"].as_table()) {
    auto& theme = config.theme;
    if (auto v = (*themeTbl)["source"].value<std::string>()) {
      if (auto parsed = enumFromKey(kPaletteSources, *v)) {
        theme.source = *parsed;
      }
    }
    if (auto builtin = (*themeTbl)["builtin"].value<std::string>()) {
      theme.builtinPalette = *builtin;
    }
    if (auto v = (*themeTbl)["community_palette"].value<std::string>()) {
      theme.communityPalette = *v;
    }
    if (auto v = (*themeTbl)["custom_palette"].value<std::string>()) {
      theme.customPalette = *v;
    }
    if (auto v = (*themeTbl)["wallpaper_scheme"].value<std::string>())
      theme.wallpaperScheme = *v;
    if (auto v = (*themeTbl)["mode"].value<std::string>()) {
      if (auto parsed = enumFromKey(kThemeModes, *v)) {
        theme.mode = *parsed;
      }
    }
    if (const auto* templatesTbl = (*themeTbl)["templates"].as_table()) {
      auto& templates = theme.templates;
      if (auto v = (*templatesTbl)["enable_builtin_templates"].value<bool>())
        templates.enableBuiltinTemplates = *v;
      if (auto v = (*templatesTbl)["enable_community_templates"].value<bool>())
        templates.enableCommunityTemplates = *v;
      if (const auto* builtinIds = (*templatesTbl)["builtin_ids"].as_array()) {
        templates.builtinIds.clear();
        templates.builtinIds.reserve(builtinIds->size());
        for (const auto& item : *builtinIds) {
          if (const auto* id = item.as_string())
            templates.builtinIds.push_back(id->get());
        }
      }
      if (const auto* communityIds = (*templatesTbl)["community_ids"].as_array()) {
        templates.communityIds.clear();
        templates.communityIds.reserve(communityIds->size());
        for (const auto& item : *communityIds) {
          if (const auto* id = item.as_string())
            templates.communityIds.push_back(id->get());
        }
      }
      if (const auto* customColorsTbl = (*templatesTbl)["custom_colors"].as_table()) {
        templates.customColors.clear();
        templates.customColors.reserve(customColorsTbl->size());
        for (const auto& [nameNode, valueNode] : *customColorsTbl) {
          ThemeConfig::TemplateColorConfig color;
          color.name = std::string(nameNode.str());
          if (const auto* str = valueNode.as_string()) {
            color.color = str->get();
          } else if (const auto* colorTbl = valueNode.as_table()) {
            if (auto value = colorTbl->get_as<std::string>("color")) {
              color.color = value->get();
            }
            if (auto blend = colorTbl->get_as<bool>("blend")) {
              color.blend = blend->get();
            }
          }
          if (!StringUtils::trim(color.name).empty() && !StringUtils::trim(color.color).empty()) {
            templates.customColors.push_back(std::move(color));
          }
        }
      }
      if (const auto* userTemplatesTbl = (*templatesTbl)["user"].as_table()) {
        templates.userTemplates.clear();
        templates.userTemplates.reserve(userTemplatesTbl->size());
        for (const auto& [idNode, templateNode] : *userTemplatesTbl) {
          const auto* templateTbl = templateNode.as_table();
          if (templateTbl == nullptr) {
            continue;
          }

          ThemeConfig::UserTemplateConfig entry;
          entry.id = std::string(idNode.str());
          if (auto enabled = templateTbl->get_as<bool>("enabled")) {
            entry.enabled = enabled->get();
          }
          if (auto inputPath = templateTbl->get_as<std::string>("input_path")) {
            entry.inputPath = inputPath->get();
          }
          if (const auto* inputPathModesTbl = (*templateTbl)["input_path_modes"].as_table()) {
            auto dark = inputPathModesTbl->get_as<std::string>("dark");
            auto light = inputPathModesTbl->get_as<std::string>("light");
            if (dark != nullptr && light != nullptr) {
              entry.inputPathModes =
                  ThemeConfig::TemplateInputPathModesConfig{.dark = dark->get(), .light = light->get()};
            }
          }
          if (const auto* outputPath = templateTbl->get("output_path")) {
            entry.outputPaths = readStringOrArray(*outputPath);
          }
          if (auto outputPathDynamic = templateTbl->get_as<std::string>("output_path_dynamic")) {
            entry.outputPathDynamic = outputPathDynamic->get();
          }
          if (auto compareTo = templateTbl->get_as<std::string>("compare_to")) {
            entry.compareTo = compareTo->get();
          }
          if (const auto* colorsToCompare = templateTbl->get("colors_to_compare")) {
            entry.colorsToCompare = readTemplateCompareColors(*colorsToCompare);
          }
          if (auto preHook = templateTbl->get_as<std::string>("pre_hook")) {
            entry.preHook = preHook->get();
          }
          if (auto postHook = templateTbl->get_as<std::string>("post_hook")) {
            entry.postHook = postHook->get();
          }
          if (auto index = templateTbl->get_as<int64_t>("index")) {
            entry.index = static_cast<int>(index->get());
          }
          if (!StringUtils::trim(entry.id).empty()) {
            templates.userTemplates.push_back(std::move(entry));
          }
        }
      }
    }
  }

  // Parse [wallpaper]
  if (auto* wpTbl = tbl["wallpaper"].as_table()) {
    auto& wp = config.wallpaper;
    if (auto v = (*wpTbl)["enabled"].value<bool>())
      wp.enabled = *v;
    if (auto v = (*wpTbl)["fill_mode"].value<std::string>()) {
      if (auto mode = enumFromKey(kWallpaperFillModes, *v)) {
        wp.fillMode = *mode;
      }
    }
    if (auto v = colorStringValue(*wpTbl, "fill_color", "wallpaper.fill_color")) {
      if (StringUtils::trim(*v).empty()) {
        wp.fillColor = std::nullopt;
      } else {
        wp.fillColor = colorSpecFromConfigString(*v, "wallpaper.fill_color");
      }
    }
    if (auto* arr = (*wpTbl)["transition"].as_array()) {
      wp.transitions.clear();
      for (const auto& item : *arr) {
        if (auto s = item.value<std::string>()) {
          if (auto t = enumFromKey(kWallpaperTransitions, *s)) {
            wp.transitions.push_back(*t);
          }
        }
      }
      if (wp.transitions.empty())
        wp.transitions.push_back(WallpaperTransition::Fade);
    }
    if (auto v = finiteDouble((*wpTbl)["transition_duration"]))
      wp.transitionDurationMs = std::clamp(static_cast<float>(*v), 100.0f, 30000.0f);
    if (auto v = finiteDouble((*wpTbl)["edge_smoothness"]))
      wp.edgeSmoothness = std::clamp(static_cast<float>(*v), 0.0f, 1.0f);
    if (auto v = (*wpTbl)["directory"].value<std::string>())
      wp.directory = expandUserPathString(*v);
    if (auto v = (*wpTbl)["directory_light"].value<std::string>())
      wp.directoryLight = expandUserPathString(*v);
    if (auto v = (*wpTbl)["directory_dark"].value<std::string>())
      wp.directoryDark = expandUserPathString(*v);
    if (auto v = (*wpTbl)["per_monitor_directories"].value<bool>())
      wp.perMonitorDirectories = *v;
    if (auto* automationTbl = (*wpTbl)["automation"].as_table()) {
      if (auto v = (*automationTbl)["enabled"].value<bool>()) {
        wp.automation.enabled = *v;
      }
      if (auto v = (*automationTbl)["interval_minutes"].value<int64_t>()) {
        wp.automation.intervalMinutes = std::clamp(static_cast<std::int32_t>(*v), 0, 1440);
      }
      if (auto v = (*automationTbl)["order"].value<std::string>()) {
        const std::string order = StringUtils::toLower(StringUtils::trim(*v));
        if (auto parsed = enumFromKey(kWallpaperAutomationOrders, order)) {
          wp.automation.order = *parsed;
        } else {
          kLog.warn("unknown wallpaper automation order \"{}\" (expected: random|alphabetical)", *v);
        }
      }
      if (auto v = (*automationTbl)["recursive"].value<bool>()) {
        wp.automation.recursive = *v;
      }
    }

    if (auto* monTblMap = (*wpTbl)["monitor"].as_table()) {
      for (const auto& [monName, monNode] : *monTblMap) {
        auto* monTbl = monNode.as_table();
        if (monTbl == nullptr) {
          continue;
        }
        WallpaperMonitorOverride ovr;
        if (auto v = (*monTbl)["match"].value<std::string>())
          ovr.match = *v;
        else
          ovr.match = std::string(monName.str());
        if (auto v = (*monTbl)["enabled"].value<bool>())
          ovr.enabled = *v;
        const std::string monitorContext = "wallpaper.monitor." + std::string(monName.str());
        if (auto v = colorStringValue(*monTbl, "fill_color", monitorContext + ".fill_color")) {
          if (StringUtils::trim(*v).empty()) {
            ovr.fillColor = std::nullopt;
          } else {
            ovr.fillColor = colorSpecFromConfigString(*v, monitorContext + ".fill_color");
          }
        }
        if (auto v = (*monTbl)["directory"].value<std::string>())
          ovr.directory = expandUserPathString(*v);
        if (auto v = (*monTbl)["directory_light"].value<std::string>())
          ovr.directoryLight = expandUserPathString(*v);
        if (auto v = (*monTbl)["directory_dark"].value<std::string>())
          ovr.directoryDark = expandUserPathString(*v);
        wp.monitorOverrides.push_back(std::move(ovr));
      }
    }
  }

  // Parse [backdrop]
  if (auto* ovTbl = tbl["backdrop"].as_table()) {
    auto& ov = config.backdrop;
    if (auto v = (*ovTbl)["enabled"].value<bool>())
      ov.enabled = *v;
    if (auto v = finiteDouble((*ovTbl)["blur_intensity"]))
      ov.blurIntensity = std::clamp(static_cast<float>(*v), 0.0f, 1.0f);
    if (auto v = finiteDouble((*ovTbl)["tint_intensity"]))
      ov.tintIntensity = std::clamp(static_cast<float>(*v), 0.0f, 1.0f);
  }

  // Parse [osd]
  if (auto* osdTbl = tbl["osd"].as_table()) {
    auto& osd = config.osd;
    if (auto v = (*osdTbl)["position"].value<std::string>())
      osd.position = *v;
    if (auto v = (*osdTbl)["orientation"].value<std::string>())
      osd.orientation = *v;
    if (auto v = finiteDouble((*osdTbl)["scale"])) {
      osd.scale = std::clamp(static_cast<float>(*v), 0.5f, 2.5f);
    }
    if (auto v = (*osdTbl)["lock_keys"].value<bool>())
      osd.lockKeys = *v;
    if (auto v = (*osdTbl)["keyboard_layout"].value<bool>())
      osd.keyboardLayout = *v;
  }

  auto parseNotificationTable = [&config](const toml::table& notifTable) {
    auto& notif = config.notification;
    if (auto v = notifTable["enable_daemon"].value<bool>())
      notif.enableDaemon = *v;
    if (auto v = notifTable["position"].value<std::string>())
      notif.position = *v;
    if (auto v = notifTable["layer"].value<std::string>())
      notif.layer = *v;
    if (auto v = finiteDouble(notifTable["scale"]))
      notif.scale = std::clamp(static_cast<float>(*v), 0.5f, 2.5f);
    if (auto v = finiteDouble(notifTable["background_opacity"]))
      notif.backgroundOpacity = std::clamp(static_cast<float>(*v), 0.0f, 1.0f);
    if (auto v = notifTable["offset_x"].value<int64_t>())
      notif.offsetX = static_cast<int>(*v);
    if (auto v = notifTable["offset_y"].value<int64_t>())
      notif.offsetY = static_cast<int>(*v);
    if (const auto* v = notifTable.get("monitors")) {
      notif.monitors = readStringArray(*v);
    }
  };

  if (auto* notifTbl = tbl["notification"].as_table()) {
    parseNotificationTable(*notifTbl);
  }
  // Compatibility alias: accept [notifications] as well.
  if (auto* notifTbl = tbl["notifications"].as_table()) {
    parseNotificationTable(*notifTbl);
  }

  // Parse [dock]
  if (auto* dockTbl = tbl["dock"].as_table()) {
    auto& dock = config.dock;
    if (auto v = (*dockTbl)["enabled"].value<bool>())
      dock.enabled = *v;
    if (auto v = (*dockTbl)["active_monitor_only"].value<bool>())
      dock.activeMonitorOnly = *v;
    if (auto v = (*dockTbl)["position"].value<std::string>())
      dock.position = *v;
    if (auto v = (*dockTbl)["icon_size"].value<int64_t>())
      dock.iconSize = std::clamp(static_cast<std::int32_t>(*v), 16, 256);
    if (auto v = (*dockTbl)["padding"].value<int64_t>())
      dock.padding = std::clamp(static_cast<std::int32_t>(*v), 0, 100);
    if (auto v = (*dockTbl)["item_spacing"].value<int64_t>())
      dock.itemSpacing = std::clamp(static_cast<std::int32_t>(*v), 0, 100);
    if (auto v = finiteDouble((*dockTbl)["background_opacity"]))
      dock.backgroundOpacity = std::clamp(static_cast<float>(*v), 0.0f, 1.0f);
    if (auto v = (*dockTbl)["radius"].value<int64_t>()) {
      const auto r = std::clamp(static_cast<std::int32_t>(*v), 0, 500);
      dock.radius = r;
      dock.radiusTopLeft = r;
      dock.radiusTopRight = r;
      dock.radiusBottomLeft = r;
      dock.radiusBottomRight = r;
    }
    if (auto v = (*dockTbl)["radius_top_left"].value<int64_t>())
      dock.radiusTopLeft = std::clamp(static_cast<std::int32_t>(*v), 0, 500);
    if (auto v = (*dockTbl)["radius_top_right"].value<int64_t>())
      dock.radiusTopRight = std::clamp(static_cast<std::int32_t>(*v), 0, 500);
    if (auto v = (*dockTbl)["radius_bottom_left"].value<int64_t>())
      dock.radiusBottomLeft = std::clamp(static_cast<std::int32_t>(*v), 0, 500);
    if (auto v = (*dockTbl)["radius_bottom_right"].value<int64_t>())
      dock.radiusBottomRight = std::clamp(static_cast<std::int32_t>(*v), 0, 500);
    if (auto v = (*dockTbl)["margin_ends"].value<int64_t>())
      dock.marginEnds = std::clamp(static_cast<std::int32_t>(*v), 0, 500);
    if (auto v = (*dockTbl)["margin_edge"].value<int64_t>())
      dock.marginEdge = std::clamp(static_cast<std::int32_t>(*v), 0, 100);
    if (auto v = (*dockTbl)["shadow"].value<bool>())
      dock.shadow = *v;
    if (auto v = (*dockTbl)["show_running"].value<bool>())
      dock.showRunning = *v;
    if (auto v = (*dockTbl)["auto_hide"].value<bool>())
      dock.autoHide = *v;
    if (auto v = (*dockTbl)["reserve_space"].value<bool>())
      dock.reserveSpace = *v;
    if (auto v = finiteDouble((*dockTbl)["active_scale"]))
      dock.activeScale = std::clamp(static_cast<float>(*v), 0.1f, 1.75f);
    if (auto v = finiteDouble((*dockTbl)["inactive_scale"]))
      dock.inactiveScale = std::clamp(static_cast<float>(*v), 0.1f, 1.0f);
    if (auto v = finiteDouble((*dockTbl)["active_opacity"]))
      dock.activeOpacity = std::clamp(static_cast<float>(*v), 0.0f, 1.0f);
    if (auto v = finiteDouble((*dockTbl)["inactive_opacity"]))
      dock.inactiveOpacity = std::clamp(static_cast<float>(*v), 0.0f, 1.0f);
    if (auto v = (*dockTbl)["show_dots"].value<bool>())
      dock.showDots = *v;
    if (auto v = (*dockTbl)["show_instance_count"].value<bool>())
      dock.showInstanceCount = *v;
    if (auto v = (*dockTbl)["launcher_position"].value<std::string>()) {
      if (*v == "none" || *v == "start" || *v == "end") {
        dock.launcherPosition = *v;
      } else {
        kLog.warn("invalid dock.launcher_position '{}'; expected none, start, or end", *v);
      }
    }
    if (auto v = (*dockTbl)["launcher_icon"].value<std::string>())
      dock.launcherIcon = *v;
    if (auto* arr = (*dockTbl)["pinned"].as_array())
      dock.pinned = readStringArray(*arr);
    if (auto* arr = (*dockTbl)["monitors"].as_array())
      dock.monitors = readStringArray(*arr);
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
    if (const auto* gridTable = (*desktopWidgetsTbl)["grid"].as_table()) {
      if (auto visible = (*gridTable)["visible"].value<bool>()) {
        desktopWidgets.grid.visible = *visible;
      }
      if (auto cellSize = (*gridTable)["cell_size"].value<int64_t>()) {
        desktopWidgets.grid.cellSize = std::clamp(static_cast<std::int32_t>(*cellSize), 8, 256);
      }
      if (auto majorInterval = (*gridTable)["major_interval"].value<int64_t>()) {
        desktopWidgets.grid.majorInterval = std::clamp(static_cast<std::int32_t>(*majorInterval), 1, 16);
      }
    }
    if (const auto* widgetsTable = (*desktopWidgetsTbl)["widget"].as_table()) {
      std::vector<DesktopWidgetState> parsedWidgets;
      parsedWidgets.reserve(widgetsTable->size());
      for (const auto& [idNode, widgetNode] : *widgetsTable) {
        const auto* widgetTable = widgetNode.as_table();
        if (widgetTable == nullptr) {
          continue;
        }
        auto widget = readDesktopWidgetState(idNode.str(), *widgetTable);
        if (!widget.id.empty() && !widget.type.empty()) {
          parsedWidgets.push_back(std::move(widget));
        }
      }

      std::vector<std::string> order;
      bool orderSpecified = false;
      if (const auto* orderNode = desktopWidgetsTbl->get("widget_order")) {
        order = readStringArray(*orderNode);
        orderSpecified = true;
      }

      desktopWidgets.widgets.clear();
      std::vector<bool> used(parsedWidgets.size(), false);
      for (const auto& orderedId : order) {
        for (std::size_t i = 0; i < parsedWidgets.size(); ++i) {
          if (!used[i] && parsedWidgets[i].id == orderedId) {
            used[i] = true;
            desktopWidgets.widgets.push_back(std::move(parsedWidgets[i]));
            break;
          }
        }
      }
      if (!orderSpecified) {
        for (std::size_t i = 0; i < parsedWidgets.size(); ++i) {
          if (!used[i]) {
            desktopWidgets.widgets.push_back(std::move(parsedWidgets[i]));
          }
        }
      }
    }
  }

  // Parse [weather]
  if (auto* weatherTbl = tbl["weather"].as_table()) {
    auto& weather = config.weather;
    if (auto v = (*weatherTbl)["enabled"].value<bool>())
      weather.enabled = *v;
    if (auto v = (*weatherTbl)["auto_locate"].value<bool>())
      weather.autoLocate = *v;
    if (auto v = (*weatherTbl)["effects"].value<bool>())
      weather.effects = *v;
    if (auto v = (*weatherTbl)["address"].value<std::string>())
      weather.address = *v;
    if (auto v = (*weatherTbl)["refresh_minutes"].value<int64_t>())
      weather.refreshMinutes = static_cast<std::int32_t>(*v);
    if (auto v = (*weatherTbl)["unit"].value<std::string>())
      weather.unit = *v;
  }

  // Parse [system]
  if (auto* systemTbl = tbl["system"].as_table()) {
    auto& system = config.system;
    if (const auto* monitorTbl = (*systemTbl)["monitor"].as_table()) {
      auto& monitor = system.monitor;
      if (auto v = (*monitorTbl)["enabled"].value<bool>()) {
        monitor.enabled = *v;
      }
      if (auto v = finiteDouble((*monitorTbl)["cpu_poll_seconds"])) {
        monitor.cpuPollSeconds = static_cast<float>(*v);
      }
      if (auto v = finiteDouble((*monitorTbl)["gpu_poll_seconds"])) {
        monitor.gpuPollSeconds = static_cast<float>(*v);
      }
      if (auto v = finiteDouble((*monitorTbl)["memory_poll_seconds"])) {
        monitor.memoryPollSeconds = static_cast<float>(*v);
      }
      if (auto v = finiteDouble((*monitorTbl)["network_poll_seconds"])) {
        monitor.networkPollSeconds = static_cast<float>(*v);
      }
      if (auto v = finiteDouble((*monitorTbl)["disk_poll_seconds"])) {
        monitor.diskPollSeconds = static_cast<float>(*v);
      }
    }
  }

  // Parse [audio]
  if (auto* audioTbl = tbl["audio"].as_table()) {
    auto& audio = config.audio;
    if (auto v = (*audioTbl)["enable_overdrive"].value<bool>()) {
      audio.enableOverdrive = *v;
    }
    if (auto v = (*audioTbl)["enable_sounds"].value<bool>()) {
      audio.enableSounds = *v;
    }
    if (auto v = finiteDouble((*audioTbl)["sound_volume"])) {
      audio.soundVolume = std::clamp(static_cast<float>(*v), 0.0f, 1.0f);
    }
    if (auto v = (*audioTbl)["volume_change_sound"].value<std::string>()) {
      audio.volumeChangeSound = *v;
    }
    if (auto v = (*audioTbl)["notification_sound"].value<std::string>()) {
      audio.notificationSound = *v;
    }
  }

  // Parse [brightness]
  if (auto* brightnessTbl = tbl["brightness"].as_table()) {
    auto& brightness = config.brightness;
    if (auto v = (*brightnessTbl)["enable_ddcutil"].value<bool>()) {
      brightness.enableDdcutil = *v;
    }
    if (auto* mmidArr = (*brightnessTbl)["ignore_mmids"].as_array()) {
      for (const auto& item : *mmidArr) {
        if (auto s = item.value<std::string>()) {
          brightness.ddcutilIgnoreMmids.push_back(*s);
        }
      }
    }
    if (auto* monitorTblMap = (*brightnessTbl)["monitor"].as_table()) {
      for (const auto& [name, node] : *monitorTblMap) {
        auto* entryTbl = node.as_table();
        if (entryTbl == nullptr) {
          continue;
        }

        BrightnessMonitorOverride override;
        override.match = std::string(name.str());

        if (auto v = (*entryTbl)["match"].value<std::string>()) {
          override.match = *v;
        }
        if (auto v = (*entryTbl)["backend"].value<std::string>()) {
          if (const auto parsed = enumFromKey(kBrightnessBackendPreferences, StringUtils::trim(*v));
              parsed.has_value()) {
            override.backend = *parsed;
          } else {
            kLog.warn("invalid brightness backend '{}' for monitor override '{}'", *v, override.match);
          }
        }

        brightness.monitorOverrides.push_back(std::move(override));
      }
    }
  }

  // Parse [keybinds]
  if (auto* keybindsTbl = tbl["keybinds"].as_table()) {
    auto& keybinds = config.keybinds;

    auto parseAction = [&](std::string_view key, std::vector<KeyChord>& out) {
      out.clear();
      if (const auto* node = keybindsTbl->get(key)) {
        if (const auto v = node->value<std::string>()) {
          try {
            if (const auto chord = parseKeyChordSpec(*v); chord.has_value()) {
              out.push_back(*chord);
            } else {
              kLog.warn("invalid keybind chord for [{}] {} = \"{}\"", "keybinds", key, *v);
            }
          } catch (const std::exception& e) {
            throw std::runtime_error(std::format("keybinds.{}: {}", key, e.what()));
          }
          return;
        }
        if (const auto* arr = node->as_array()) {
          for (const auto& item : *arr) {
            if (const auto v = item.value<std::string>()) {
              try {
                if (const auto chord = parseKeyChordSpec(*v); chord.has_value()) {
                  out.push_back(*chord);
                } else {
                  kLog.warn("invalid keybind chord for [{}] {} item = \"{}\"", "keybinds", key, *v);
                }
              } catch (const std::exception& e) {
                throw std::runtime_error(std::format("keybinds.{}: {}", key, e.what()));
              }
            }
          }
        }
      }
    };

    parseAction("validate", keybinds.validate);
    parseAction("cancel", keybinds.cancel);
    parseAction("left", keybinds.left);
    parseAction("right", keybinds.right);
    parseAction("up", keybinds.up);
    parseAction("down", keybinds.down);
  }

  // Parse [nightlight]
  if (auto* nightlightTbl = tbl["nightlight"].as_table()) {
    auto& nightlight = config.nightlight;
    if (auto v = (*nightlightTbl)["enabled"].value<bool>()) {
      nightlight.enabled = *v;
    }
    if (auto v = (*nightlightTbl)["force"].value<bool>()) {
      nightlight.force = *v;
    }
    if (auto v = (*nightlightTbl)["use_weather_location"].value<bool>()) {
      nightlight.useWeatherLocation = *v;
    }
    if (auto v = (*nightlightTbl)["start_time"].value<std::string>()) {
      nightlight.startTime = *v;
    }
    if (auto v = (*nightlightTbl)["stop_time"].value<std::string>()) {
      nightlight.stopTime = *v;
    }
    if (auto v = finiteDouble((*nightlightTbl)["latitude"])) {
      nightlight.latitude = *v;
    }
    if (auto v = finiteDouble((*nightlightTbl)["longitude"])) {
      nightlight.longitude = *v;
    }
    if (auto v = (*nightlightTbl)["temperature_day"].value<int64_t>()) {
      nightlight.dayTemperature = std::clamp(static_cast<std::int32_t>(*v), 1000, 25000);
    }
    if (auto v = (*nightlightTbl)["temperature_night"].value<int64_t>()) {
      nightlight.nightTemperature = std::clamp(static_cast<std::int32_t>(*v), 1000, 25000);
    }
    if (nightlight.dayTemperature - nightlight.nightTemperature < NightLightConfig::kTemperatureGap) {
      const std::int32_t origDay = nightlight.dayTemperature;
      const std::int32_t origNight = nightlight.nightTemperature;
      // Prefer to preserve day and pull night down; if day is too low to leave room for the gap, bump day up.
      nightlight.nightTemperature = origDay - NightLightConfig::kTemperatureGap;
      if (nightlight.nightTemperature < NightLightConfig::kTemperatureMin) {
        nightlight.nightTemperature = NightLightConfig::kTemperatureMin;
        nightlight.dayTemperature = NightLightConfig::kTemperatureMin + NightLightConfig::kTemperatureGap;
      }
      kLog.warn(
          "nightlight temperatures must satisfy day > night (day={}K night={}K); adjusted to day={}K night={}K",
          origDay, origNight, nightlight.dayTemperature, nightlight.nightTemperature
      );
    }
  }

  // Parse [hooks]
  if (auto* hooksTbl = tbl["hooks"].as_table()) {
    auto& hooks = config.hooks;
    for (const auto& [name, node] : *hooksTbl) {
      const std::string_view keyView{name.str()};
      if (keyView == "battery_low_percent_threshold") {
        if (auto v = node.value<int64_t>()) {
          hooks.batteryLowPercentThreshold =
              static_cast<std::int32_t>(std::clamp(*v, static_cast<std::int64_t>(0), static_cast<std::int64_t>(100)));
        }
        continue;
      }
      if (const auto kind = hookKindFromKey(keyView)) {
        setHookCommandsFromNode(node, hooks.commands[static_cast<std::size_t>(*kind)]);
      }
    }
  }

  // Parse [[control_center.shortcuts]]
  bool controlCenterShortcutsConfigured = false;
  if (auto* ccTbl = tbl["control_center"].as_table()) {
    if (auto v = (*ccTbl)["compact"].value<bool>()) {
      config.controlCenter.compact = *v;
    }
    if (auto* shortcutsArr = (*ccTbl)["shortcuts"].as_array()) {
      controlCenterShortcutsConfigured = true;
      config.controlCenter.shortcuts.clear();
      for (const auto& entry : *shortcutsArr) {
        auto* entryTbl = entry.as_table();
        if (entryTbl == nullptr) {
          continue;
        }
        ShortcutConfig sc;
        if (auto v = (*entryTbl)["type"].value<std::string>()) {
          sc.type = *v;
        }
        if (!sc.type.empty()) {
          config.controlCenter.shortcuts.push_back(std::move(sc));
        }
      }
    }
  }
  if (!controlCenterShortcutsConfigured && config.controlCenter.shortcuts.empty()) {
    config.controlCenter.shortcuts = defaultControlCenterShortcuts();
  }

  // Parse [idle] and [idle.behavior.*]
  if (auto* idleTbl = tbl["idle"].as_table()) {
    if (auto v = finiteDouble((*idleTbl)["pre_action_fade_seconds"])) {
      const double d = *v;
      config.idle.preActionFadeSeconds = static_cast<float>(std::clamp(d, 0.0, 120.0));
    }
    if (auto* behaviorTbl = (*idleTbl)["behavior"].as_table()) {
      for (const auto& [name, node] : *behaviorTbl) {
        auto* entryTbl = node.as_table();
        if (entryTbl == nullptr) {
          continue;
        }

        IdleBehaviorConfig behavior;
        behavior.name = std::string(name.str());

        if (auto v = (*entryTbl)["enabled"].value<bool>()) {
          behavior.enabled = *v;
        }
        if (auto v = (*entryTbl)["timeout"].value<int64_t>()) {
          behavior.timeoutSeconds = static_cast<std::int32_t>(*v);
        }
        if (auto v = (*entryTbl)["action"].value<std::string>()) {
          behavior.action = StringUtils::trim(*v);
        }
        if (auto v = (*entryTbl)["command"].value<std::string>()) {
          behavior.command = *v;
        }
        if (auto v = (*entryTbl)["resume_command"].value<std::string>()) {
          behavior.resumeCommand = *v;
        }
        if (auto v = (*entryTbl)["lock_before_suspend"].value<bool>()) {
          behavior.lockBeforeSuspend = *v;
        }

        inferIdleBehaviorActionFromLegacyFields(behavior);

        config.idle.behaviors.push_back(std::move(behavior));
      }
    }
    if (auto* orderArr = (*idleTbl)["behavior_order"].as_array();
        orderArr != nullptr && !config.idle.behaviors.empty()) {
      std::vector<std::string> orderedNames;
      orderedNames.reserve(orderArr->size());
      for (const auto& item : *orderArr) {
        if (auto name = item.value<std::string>(); name.has_value() && !name->empty()) {
          orderedNames.push_back(*name);
        }
      }

      if (!orderedNames.empty()) {
        std::unordered_map<std::string, IdleBehaviorConfig> byName;
        byName.reserve(config.idle.behaviors.size());
        for (auto& behavior : config.idle.behaviors) {
          byName.insert_or_assign(behavior.name, std::move(behavior));
        }

        std::vector<IdleBehaviorConfig> ordered;
        ordered.reserve(byName.size());
        for (const auto& name : orderedNames) {
          auto it = byName.find(name);
          if (it == byName.end()) {
            continue;
          }
          ordered.push_back(std::move(it->second));
          byName.erase(it);
        }
        for (auto& [name, behavior] : byName) {
          (void)name;
          ordered.push_back(std::move(behavior));
        }

        config.idle.behaviors = std::move(ordered);
      }
    }
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
    kLog.info(
        "hooks kinds with commands={} battery_low_threshold={}%", hookKindsUsed, config.hooks.batteryLowPercentThreshold
    );
  }
}

bool ConfigService::matchesKeybind(KeybindAction action, std::uint32_t sym, std::uint32_t modifiers) const {
  const auto& configured = keybindSet(m_config.keybinds, action);
  const auto active = configured.empty() ? defaultKeybindSet(action) : configured;
  return std::any_of(active.begin(), active.end(), [sym, modifiers](const KeyChord& chord) {
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
