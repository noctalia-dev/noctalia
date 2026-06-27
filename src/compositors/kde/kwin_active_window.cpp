#include "compositors/kde/kwin_active_window.h"

#include "core/log.h"
#include "dbus/session_bus.h"
#include "system/app_identity.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <json.hpp>
#include <sdbus-c++/sdbus-c++.h>
#include <unordered_set>

namespace {

  constexpr Logger kLog("kwin_active_window");

  const sdbus::ServiceName kBusName{"dev.noctalia.KWinActiveWindow"};
  const sdbus::ObjectPath kObjectPath{"/dev/noctalia/KWinActiveWindow"};
  constexpr auto kInterface = "dev.noctalia.KWinActiveWindow";
  constexpr auto kScriptLabel = "noctalia-active-window";

  const sdbus::ServiceName kKwinBusName{"org.kde.KWin"};
  const sdbus::ObjectPath kKwinPath{"/KWin"};
  const sdbus::ObjectPath kKwinScriptingPath{"/Scripting"};
  constexpr auto kKwinInterface = "org.kde.KWin";
  constexpr auto kKwinScriptingInterface = "org.kde.kwin.Scripting";

  constexpr char kRecordSeparator = '\x1f';
  constexpr char kFieldSeparator = '\x1e';

  constexpr std::string_view kScriptSource = R"js(
const BUS = "dev.noctalia.KWinActiveWindow";
const PATH = "/dev/noctalia/KWinActiveWindow";
const IFACE = "dev.noctalia.KWinActiveWindow";
const RECORD_SEP = "\x1f";
const FIELD_SEP = "\x1e";

function windowUuid(window) {
  if (!window || window.internalId === undefined) {
    return "";
  }
  return String(window.internalId);
}

function notify(window) {
  try {
    if (!window) {
      callDBus(BUS, PATH, IFACE, "notifyActiveWindow", "", "", "");
      return;
    }
    if (!shouldTrack(window)) {
      return;
    }
    const caption = window.caption || "";
    const resourceClass = window.resourceClass || "";
    callDBus(BUS, PATH, IFACE, "notifyActiveWindow", caption, resourceClass, windowUuid(window));
  } catch (error) {
    print("noctalia-active-window notify failed: " + error);
  }
}

function isNoctaliaShellSurface(window) {
  const resourceClass = (window.resourceClass || "").toLowerCase();
  if (resourceClass === "noctalia") {
    return true;
  }
  const caption = (window.caption || "").toLowerCase();
  return caption === "noctalia" && resourceClass === "";
}

function shouldTrack(window) {
  if (!window) {
    return false;
  }
  if (window.skipTaskbar) {
    return false;
  }
  if (window.dock || window.desktopWindow || window.tooltip || window.notification) {
    return false;
  }
  if (!window.normalWindow) {
    return false;
  }
  if (window.dialog || window.splash || window.utility || window.dropdownMenu || window.popupMenu) {
    return false;
  }
  if (isNoctaliaShellSurface(window)) {
    return false;
  }
  return true;
}

function canonicalDesktopId(desktop) {
  if (!desktop) {
    return "";
  }
  const id = desktop.id !== undefined && desktop.id !== null ? String(desktop.id) : "";
  if (!id) {
    return "";
  }
  try {
    const ids = virtualDesktopInfo.desktopIds;
    if (!ids || ids.length === 0) {
      return id;
    }
    for (let i = 0; i < ids.length; ++i) {
      if (String(ids[i]) === id) {
        return String(ids[i]);
      }
    }
    const asNum = parseInt(id, 10);
    if (!isNaN(asNum) && asNum > 0 && asNum <= ids.length) {
      return String(ids[asNum - 1]);
    }
  } catch (error) {
  }
  return id;
}

function desktopIdsFor(window) {
  if (!window) {
    return "";
  }
  if (window.onAllDesktops) {
    return "*";
  }
  const desktops = window.desktops;
  if (!desktops || desktops.length === 0) {
    return "";
  }
  const ids = [];
  for (let i = 0; i < desktops.length; ++i) {
    const id = canonicalDesktopId(desktops[i]);
    if (id) {
      ids.push(id);
    }
  }
  return ids.join(",");
}

function outputNameFor(window) {
  if (!window || window.output === undefined || window.output === null) {
    return "";
  }
  return window.output.name || "";
}

function serializeWindow(window) {
  if (!shouldTrack(window)) {
    return "";
  }
  const uuid = windowUuid(window);
  if (!uuid) {
    return "";
  }
  return [
    uuid,
    window.resourceClass || "",
    window.caption || "",
    desktopIdsFor(window),
    outputNameFor(window),
  ].join(FIELD_SEP);
}

function syncWindows() {
  try {
    const rows = [];
    for (const window of workspace.windowList()) {
      const row = serializeWindow(window);
      if (row) {
        rows.push(row);
      }
    }
    callDBus(BUS, PATH, IFACE, "notifyWindowList", rows.join(RECORD_SEP));
  } catch (error) {
    print("noctalia-active-window sync failed: " + error);
  }
}

function wireWindow(window) {
  if (!window) {
    return;
  }
  const updateActive = () => {
    if (workspace.activeWindow === window) {
      notify(window);
    }
  };
  const updateTracked = () => {
    syncWindows();
  };
  if (typeof window.captionChanged !== "undefined") {
    window.captionChanged.connect(updateActive);
    window.captionChanged.connect(updateTracked);
  }
  if (typeof window.desktopsChanged !== "undefined") {
    window.desktopsChanged.connect(updateTracked);
  }
  if (typeof window.outputChanged !== "undefined") {
    window.outputChanged.connect(updateTracked);
  }
}

workspace.windowActivated.connect(notify);
workspace.windowAdded.connect((window) => {
  wireWindow(window);
  syncWindows();
});
workspace.windowRemoved.connect(syncWindows);

for (const window of workspace.windowList()) {
  wireWindow(window);
}

notify(workspace.activeWindow);
syncWindows();
)js";

  [[nodiscard]] bool writeScriptFile(const std::filesystem::path& path, std::string_view source) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      kLog.warn("failed to create kwin script directory {}: {}", path.parent_path().string(), ec.message());
      return false;
    }

    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out.is_open()) {
      kLog.warn("failed to open kwin script for writing: {}", path.string());
      return false;
    }
    out.write(source.data(), static_cast<std::streamsize>(source.size()));
    if (!out) {
      kLog.warn("failed to write kwin script: {}", path.string());
      return false;
    }
    return true;
  }

  [[nodiscard]] std::string jsStringLiteral(const std::string& value) { return nlohmann::json(value).dump(); }

  [[nodiscard]] std::vector<std::string> splitString(std::string_view value, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= value.size()) {
      const std::size_t end = value.find(separator, start);
      if (end == std::string_view::npos) {
        parts.emplace_back(value.substr(start));
        break;
      }
      parts.emplace_back(value.substr(start, end - start));
      start = end + 1;
    }
    return parts;
  }

  [[nodiscard]] bool isNoctaliaShellSurface(const std::string& appId, const std::string& title) {
    const std::string appLower = StringUtils::toLower(appId);
    if (appLower == "noctalia") {
      return true;
    }
    if (appLower.empty() && StringUtils::toLower(title) == "noctalia") {
      return true;
    }
    return false;
  }

} // namespace

namespace compositors::kde {

  KwinActiveWindow::KwinActiveWindow(SessionBus& bus) : m_bus(bus) {}

  KwinActiveWindow::~KwinActiveWindow() { stop(); }

  void KwinActiveWindow::start() {
    if (m_started) {
      return;
    }

    try {
      m_bus.connection().requestName(kBusName);
    } catch (const sdbus::Error& e) {
      kLog.warn("failed to claim {}: {}", std::string{kBusName}, e.getMessage());
      return;
    }

    try {
      m_object = sdbus::createObject(m_bus.connection(), kObjectPath);
      m_object
          ->addVTable(
              sdbus::registerMethod("notifyActiveWindow")
                  .withInputParamNames("caption", "resourceClass", "uuid")
                  .implementedAs(
                      [this](const std::string& caption, const std::string& resourceClass, const std::string& uuid) {
                        onNotifyActiveWindow(caption, resourceClass, uuid);
                      }
                  ),
              sdbus::registerMethod("notifyWindowList")
                  .withInputParamNames("payload")
                  .implementedAs([this](const std::string& payload) { onNotifyWindowList(payload); })
          )
          .forInterface(kInterface);
    } catch (const sdbus::Error& e) {
      kLog.warn("failed to export kwin active-window dbus object: {}", e.getMessage());
      m_object.reset();
      return;
    }

    std::string scriptPath;
    if (!ensureScriptFile(scriptPath)) {
      return;
    }
    if (!installScript(scriptPath)) {
      return;
    }

    m_scriptInstalled = true;
    m_started = true;
  }

  void KwinActiveWindow::stop() {
    if (m_scriptInstalled) {
      uninstallScript();
      m_scriptInstalled = false;
    }
    m_object.reset();
    m_current.reset();
    m_trackedWindows.clear();
    m_started = false;
  }

  void KwinActiveWindow::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

  std::optional<ActiveToplevel> KwinActiveWindow::current() const { return m_current; }

  std::vector<std::string> KwinActiveWindow::runningAppIds() const {
    std::vector<std::string> ids;
    std::unordered_set<std::string> seen;
    ids.reserve(m_trackedWindows.size());
    for (const auto& window : m_trackedWindows) {
      if (window.appId.empty()
          || isNoctaliaShellSurface(window.appId, window.title)
          || !seen.insert(window.appId).second) {
        continue;
      }
      ids.push_back(window.appId);
    }
    return ids;
  }

  std::vector<ToplevelInfo>
  KwinActiveWindow::windowsForApp(const std::string& idLower, const std::string& wmClassLower) const {
    std::vector<ToplevelInfo> matched;
    matched.reserve(m_trackedWindows.size());
    std::uint64_t order = 0;
    for (const auto& window : m_trackedWindows) {
      if (window.appId.empty() || isNoctaliaShellSurface(window.appId, window.title)) {
        continue;
      }
      std::string appLower = window.appId;
      for (auto& c : appLower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      if (!app_identity::matchesLower(appLower, idLower, wmClassLower, {})) {
        continue;
      }
      matched.push_back(
          ToplevelInfo{
              .title = StringUtils::windowTitleSingleLine(window.title),
              .appId = window.appId,
              .identifier = window.uuid,
              .order = order++,
              .handle = nullptr,
              .extHandle = nullptr,
          }
      );
    }
    return matched;
  }

  std::vector<WorkspaceWindow> KwinActiveWindow::trackedWorkspaceWindows() const {
    std::vector<WorkspaceWindow> windows;
    for (const auto& window : m_trackedWindows) {
      if (window.uuid.empty()) {
        continue;
      }
      const std::string title = StringUtils::windowTitleSingleLine(window.title);
      const std::string appId = window.appId;
      if (appId.empty() || isNoctaliaShellSurface(appId, title)) {
        continue;
      }
      if (window.desktopIds.empty()) {
        windows.push_back(
            WorkspaceWindow{
                .windowId = window.uuid,
                .workspaceKey = {},
                .appId = appId,
                .title = title,
                .outputName = window.outputName,
            }
        );
        continue;
      }
      bool onAllDesktops = false;
      for (const auto& desktopId : window.desktopIds) {
        if (desktopId == "*") {
          onAllDesktops = true;
          break;
        }
      }
      if (onAllDesktops) {
        windows.push_back(
            WorkspaceWindow{
                .windowId = window.uuid,
                .workspaceKey = {},
                .appId = appId,
                .title = title,
                .outputName = window.outputName,
            }
        );
        continue;
      }
      for (const auto& desktopId : window.desktopIds) {
        windows.push_back(
            WorkspaceWindow{
                .windowId = window.uuid,
                .workspaceKey = desktopId,
                .appId = window.appId,
                .title = title,
                .outputName = window.outputName,
            }
        );
      }
    }
    return windows;
  }

  std::optional<std::string> KwinActiveWindow::focusedOutputName() const {
    if (!m_started) {
      return std::nullopt;
    }

    try {
      auto proxy = sdbus::createProxy(m_bus.connection(), kKwinBusName, kKwinPath);
      std::string name;
      proxy->callMethod("activeOutputName").onInterface(kKwinInterface).storeResultsTo(name);
      name = StringUtils::trim(name);
      if (!name.empty()) {
        return name;
      }
    } catch (const sdbus::Error& e) {
      kLog.debug("kwin activeOutputName unavailable: {}", e.getMessage());
    }
    return std::nullopt;
  }

  void KwinActiveWindow::activateWindow(const std::string& title, const std::string& appId, const std::string& uuid) {
    if (!m_scriptInstalled) {
      return;
    }

    const std::string script = std::format(
        R"js(
const targetUuid = {};
const targetClass = {};
const targetCaption = {};
for (const window of workspace.windowList()) {{
  if (!window || !window.normalWindow) {{
    continue;
  }}
  const uuid = window.internalId === undefined ? "" : String(window.internalId);
  if (targetUuid && uuid === targetUuid) {{
    workspace.activeWindow = window;
    break;
  }}
  if (window.resourceClass === targetClass && window.caption === targetCaption) {{
    workspace.activeWindow = window;
    break;
  }}
}}
)js",
        jsStringLiteral(uuid), jsStringLiteral(appId), jsStringLiteral(title)
    );

    const std::string label = std::format("noctalia-activate-{}", ++m_transientScriptSerial);
    if (!runTransientScript(script, label)) {
      kLog.warn(R"(failed to activate kde window class="{}" title="{}")", appId, title);
    }
  }

  void KwinActiveWindow::closeWindow(const std::string& title, const std::string& appId, const std::string& uuid) {
    if (!m_scriptInstalled) {
      return;
    }

    const std::string script = std::format(
        R"js(
const targetUuid = {};
const targetClass = {};
const targetCaption = {};
for (const window of workspace.windowList()) {{
  if (!window || !window.normalWindow) {{
    continue;
  }}
  const uuid = window.internalId === undefined ? "" : String(window.internalId);
  const byUuid = targetUuid && uuid === targetUuid;
  const byClassAndCaption = window.resourceClass === targetClass && window.caption === targetCaption;
  if (!byUuid && !byClassAndCaption) {{
    continue;
  }}
  if (typeof window.closeWindow === "function") {{
    window.closeWindow();
  }} else {{
    window.minimized = true;
  }}
  break;
}}
)js",
        jsStringLiteral(uuid), jsStringLiteral(appId), jsStringLiteral(title)
    );

    const std::string label = std::format("noctalia-close-{}", ++m_transientScriptSerial);
    if (!runTransientScript(script, label)) {
      kLog.warn(R"(failed to close kde window class="{}" title="{}")", appId, title);
    }
  }

  bool KwinActiveWindow::ensureScriptFile(std::string& scriptPath) const {
    const std::string stateDir = FileUtils::stateDir();
    if (stateDir.empty()) {
      kLog.warn("state directory unavailable; cannot install kwin active-window script");
      return false;
    }

    const std::filesystem::path path = std::filesystem::path(stateDir) / "kwin-active-window.js";
    if (!writeScriptFile(path, kScriptSource)) {
      return false;
    }
    scriptPath = path.string();
    return true;
  }

  bool KwinActiveWindow::installScript(const std::string& scriptPath) {
    auto scriptingProxy = [&]() { return sdbus::createProxy(m_bus.connection(), kKwinBusName, kKwinScriptingPath); };

    try {
      scriptingProxy()
          ->callMethod("unloadScript")
          .onInterface(kKwinScriptingInterface)
          .withArguments(std::string(kScriptLabel));
    } catch (const sdbus::Error& e) {
      kLog.debug("kwin unloadScript before load ignored: {}", e.getMessage());
    }

    int32_t scriptId = -1;
    try {
      scriptingProxy()
          ->callMethod("loadScript")
          .onInterface(kKwinScriptingInterface)
          .withArguments(scriptPath, std::string(kScriptLabel))
          .storeResultsTo(scriptId);
    } catch (const sdbus::Error& e) {
      kLog.warn("failed to load kwin active-window script: {}", e.getMessage());
      return false;
    }

    if (scriptId < 0) {
      kLog.warn("kwin loadScript returned {}; retrying after unload", scriptId);
      try {
        scriptingProxy()
            ->callMethod("unloadScript")
            .onInterface(kKwinScriptingInterface)
            .withArguments(std::string(kScriptLabel));
        scriptingProxy()
            ->callMethod("loadScript")
            .onInterface(kKwinScriptingInterface)
            .withArguments(scriptPath, std::string(kScriptLabel))
            .storeResultsTo(scriptId);
      } catch (const sdbus::Error& e) {
        kLog.warn("failed to reload kwin active-window script: {}", e.getMessage());
        return false;
      }
      if (scriptId < 0) {
        kLog.warn("kwin loadScript still returned {} after reload", scriptId);
        return false;
      }
    }

    try {
      scriptingProxy()->callMethod("start").onInterface(kKwinScriptingInterface);
    } catch (const sdbus::Error& e) {
      kLog.warn("failed to start kwin active-window script (id={}): {}", scriptId, e.getMessage());
      return false;
    }

    return true;
  }

  void KwinActiveWindow::uninstallScript() {
    try {
      auto proxy = sdbus::createProxy(m_bus.connection(), kKwinBusName, kKwinScriptingPath);
      proxy->callMethod("unloadScript").onInterface(kKwinScriptingInterface).withArguments(std::string(kScriptLabel));
    } catch (const sdbus::Error& e) {
      kLog.debug("kwin unloadScript on stop ignored: {}", e.getMessage());
    }
  }

  bool KwinActiveWindow::runTransientScript(const std::string& source, std::string_view label) {
    const std::string stateDir = FileUtils::stateDir();
    if (stateDir.empty()) {
      return false;
    }

    const std::filesystem::path path = std::filesystem::path(stateDir) / std::format("kwin-transient-{}.js", label);
    if (!writeScriptFile(path, source)) {
      return false;
    }

    auto scriptingProxy = sdbus::createProxy(m_bus.connection(), kKwinBusName, kKwinScriptingPath);
    const std::string labelString(label);

    try {
      scriptingProxy->callMethod("unloadScript").onInterface(kKwinScriptingInterface).withArguments(labelString);
    } catch (const sdbus::Error&) {
    }

    int32_t scriptId = -1;
    try {
      scriptingProxy->callMethod("loadScript")
          .onInterface(kKwinScriptingInterface)
          .withArguments(path.string(), labelString)
          .storeResultsTo(scriptId);
    } catch (const sdbus::Error& e) {
      kLog.debug("kwin transient loadScript failed: {}", e.getMessage());
      return false;
    }
    if (scriptId < 0) {
      return false;
    }

    const std::string scriptPath = std::format("/Scripting/Script{}", scriptId);
    try {
      auto scriptProxy = sdbus::createProxy(m_bus.connection(), kKwinBusName, sdbus::ObjectPath{scriptPath});
      scriptProxy->callMethod("run").onInterface("org.kde.kwin.Script");
    } catch (const sdbus::Error& e) {
      kLog.debug("kwin transient script run failed: {}", e.getMessage());
      return false;
    }

    try {
      scriptingProxy->callMethod("unloadScript").onInterface(kKwinScriptingInterface).withArguments(labelString);
    } catch (const sdbus::Error&) {
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
    return true;
  }

  void KwinActiveWindow::onNotifyActiveWindow(
      const std::string& caption, const std::string& resourceClass, const std::string& uuid
  ) {
    const auto before = m_current;

    if (caption.empty() && resourceClass.empty()) {
      m_current.reset();
    } else {
      const std::string title = StringUtils::windowTitleSingleLine(caption);
      const std::string& appId = resourceClass;
      if (isNoctaliaShellSurface(appId, title)) {
        return;
      }
      std::string identifier;
      if (!uuid.empty()) {
        identifier = uuid;
      } else if (!appId.empty() || !title.empty()) {
        identifier = appId + ":" + title;
      }
      m_current = ActiveToplevel{
          .title = title,
          .appId = appId,
          .identifier = identifier,
          .handle = nullptr,
      };
    }

    const bool changed = [&] {
      if (before.has_value() != m_current.has_value()) {
        return true;
      }
      if (!before.has_value() || !m_current.has_value()) {
        return false;
      }
      return before->title != m_current->title
          || before->appId != m_current->appId
          || before->identifier != m_current->identifier;
    }();
    if (changed) {
      notifyChanged();
    }
  }

  void KwinActiveWindow::onNotifyWindowList(const std::string& payload) {
    std::vector<TrackedWindow> next;
    for (const auto& record : splitString(payload, kRecordSeparator)) {
      if (record.empty()) {
        continue;
      }
      const auto fields = splitString(record, kFieldSeparator);
      if (fields.size() < 3) {
        continue;
      }
      TrackedWindow window{
          .uuid = fields[0],
          .appId = fields[1],
          .title = StringUtils::windowTitleSingleLine(fields[2]),
          .outputName = fields.size() >= 5 ? fields[4] : std::string{},
          .desktopIds = {},
      };
      if (fields.size() >= 4 && !fields[3].empty() && fields[3] != "*") {
        window.desktopIds = splitString(fields[3], ',');
      }
      if (window.uuid.empty() && window.appId.empty()) {
        continue;
      }
      if (isNoctaliaShellSurface(window.appId, window.title)) {
        continue;
      }
      next.push_back(std::move(window));
    }

    const bool changed = [&] {
      if (m_trackedWindows.size() != next.size()) {
        return true;
      }
      for (std::size_t i = 0; i < next.size(); ++i) {
        const auto& lhs = next[i];
        const auto& rhs = m_trackedWindows[i];
        if (lhs.uuid != rhs.uuid
            || lhs.appId != rhs.appId
            || lhs.title != rhs.title
            || lhs.outputName != rhs.outputName
            || lhs.desktopIds != rhs.desktopIds) {
          return true;
        }
      }
      return false;
    }();
    m_trackedWindows = std::move(next);
    if (changed) {
      notifyChanged();
    }
  }

  void KwinActiveWindow::notifyChanged() const {
    if (m_changeCallback) {
      m_changeCallback();
    }
  }

} // namespace compositors::kde
