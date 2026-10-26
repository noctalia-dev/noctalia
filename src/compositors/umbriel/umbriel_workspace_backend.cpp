#include "compositors/umbriel/umbriel_workspace_backend.h"

#include "compositors/umbriel/umbriel_runtime.h"
#include "core/log.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace {

  constexpr Logger kLog("umbriel_workspace_backend");

  [[nodiscard]] std::string jsonString(const nlohmann::json& json, std::string_view key) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_string()) {
      return {};
    }
    return it->get<std::string>();
  }

  [[nodiscard]] std::optional<WindowMinimizeState> parseMinimizeState(const nlohmann::json& entry) {
    const auto minimized = entry.find("minimized");
    const auto minimizable = entry.find("minimizable");
    if (minimized == entry.end()
        || !minimized->is_boolean()
        || minimizable == entry.end()
        || !minimizable->is_boolean()) {
      kLog.warn("windows snapshot requires boolean minimized and minimizable fields");
      return std::nullopt;
    }
    return WindowMinimizeState{
        .minimized = minimized->get<bool>(),
        .minimizable = minimizable->get<bool>(),
    };
  }

} // namespace

UmbrielWorkspaceBackend::UmbrielWorkspaceBackend(compositors::umbriel::UmbrielRuntime& runtime)
    : compositors::umbriel::UmbrielEventHandler(runtime) {}

UmbrielWorkspaceBackend::~UmbrielWorkspaceBackend() { cleanup(); }

void UmbrielWorkspaceBackend::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void UmbrielWorkspaceBackend::setWindowChangeCallback(ChangeCallback callback) {
  m_windowChangeCallback = std::move(callback);
}

void UmbrielWorkspaceBackend::setOverviewChangeCallback(ChangeCallback callback) {
  m_overviewChangeCallback = std::move(callback);
}

bool UmbrielWorkspaceBackend::canTrackOverviewState() const noexcept { return m_runtime.available(); }

int UmbrielWorkspaceBackend::pollFd() const noexcept { return m_runtime.pollFd(); }

short UmbrielWorkspaceBackend::pollEvents() const noexcept { return m_runtime.pollEvents(); }

int UmbrielWorkspaceBackend::pollTimeoutMs() const noexcept { return m_runtime.pollTimeoutMs(); }

void UmbrielWorkspaceBackend::dispatchPoll(short revents) { m_runtime.dispatchPoll(revents); }

std::string UmbrielWorkspaceBackend::workspaceKey(const std::string& id, const std::string& name) {
  return name.empty() ? id : name;
}

const UmbrielWorkspaceBackend::WorkspaceList*
UmbrielWorkspaceBackend::cachedWorkspaces(const std::string& outputName) const {
  for (const auto& [cachedOutput, workspaces] : m_workspacesByOutput) {
    if (cachedOutput == outputName) {
      return &workspaces;
    }
  }
  return nullptr;
}

std::string UmbrielWorkspaceBackend::workspaceKeyForId(const std::string& workspaceId) const {
  for (const auto& [cachedOutput, workspaces] : m_workspacesByOutput) {
    (void)cachedOutput;
    for (const auto& [id, name] : workspaces) {
      if (id == workspaceId) {
        return workspaceKey(id, name);
      }
    }
  }
  return workspaceId;
}

void UmbrielWorkspaceBackend::apply(std::vector<Workspace>& workspaces, const std::string& outputName) const {
  for (auto& workspace : workspaces) {
    workspace.occupied =
        std::ranges::any_of(m_windows, [&](const auto& window) { return window.second.workspaceId == workspace.id; });
  }

  WorkspaceList refreshed;
  refreshed.reserve(workspaces.size());
  for (const auto& workspace : workspaces) {
    refreshed.emplace_back(workspace.id, workspace.name);
  }

  for (auto& [cachedOutput, cached] : m_workspacesByOutput) {
    if (cachedOutput != outputName) {
      continue;
    }
    cached = std::move(refreshed);
    return;
  }
  m_workspacesByOutput.emplace_back(outputName, std::move(refreshed));
}

std::vector<std::string> UmbrielWorkspaceBackend::workspaceKeys(const std::string& outputName) const {
  std::vector<std::string> result;
  if (!outputName.empty()) {
    if (const auto* workspaces = cachedWorkspaces(outputName)) {
      result.reserve(workspaces->size());
      for (const auto& [id, name] : *workspaces) {
        result.push_back(workspaceKey(id, name));
      }
    }
    return result;
  }

  // Concatenation over all cached outputs in insertion order. Both this and
  // workspaces(output) derive from the same ext list, so the per-output keys
  // stay positionally aligned with the workspaces the taskbar renders.
  for (const auto& [cachedOutput, workspaces] : m_workspacesByOutput) {
    (void)cachedOutput;
    for (const auto& [id, name] : workspaces) {
      result.push_back(workspaceKey(id, name));
    }
  }
  return result;
}

std::unordered_map<std::string, std::vector<std::string>>
UmbrielWorkspaceBackend::appIdsByWorkspace(const std::string& outputName) const {
  std::unordered_map<std::string, std::vector<std::string>> result;
  std::unordered_map<std::string, std::unordered_set<std::string>> seen;
  for (const auto& window : workspaceWindows(outputName)) {
    if (window.appId.empty()) {
      continue;
    }
    auto& workspaceSeen = seen[window.workspaceKey];
    if (workspaceSeen.insert(window.appId).second) {
      result[window.workspaceKey].push_back(window.appId);
    }
  }
  return result;
}

std::vector<WorkspaceWindow> UmbrielWorkspaceBackend::workspaceWindows(const std::string& outputName) const {
  std::vector<WorkspaceWindow> result;
  result.reserve(m_windows.size());
  const std::string prefix = outputName.empty() ? std::string{} : outputName + ":";
  for (const auto& [windowId, window] : m_windows) {
    if (window.workspaceId.empty()) {
      // Scratchpad windows carry no workspace and are intentionally hidden in
      // Umbriel's UX.
      continue;
    }
    if (!prefix.empty() && !window.workspaceId.starts_with(prefix)) {
      continue;
    }
    result.push_back(
        WorkspaceWindow{
            .windowId = windowId,
            .workspaceKey = workspaceKeyForId(window.workspaceId),
            .appId = window.appId,
            .title = window.title,
            .x = window.x,
            .y = window.y,
            .outputName = {},
            .minimizeState = window.minimizeState,
        }
    );
  }
  return result;
}

bool UmbrielWorkspaceBackend::focusWindowById(const std::string& windowId, bool warpPointer) {
  return m_runtime.requestAction(std::string(warpPointer ? "window-focus-warp:" : "window-focus:") + windowId);
}

std::optional<std::string> UmbrielWorkspaceBackend::focusedWindowId() const { return m_focusedWindowId; }

bool UmbrielWorkspaceBackend::closeWindowById(const std::string& windowId) {
  return m_runtime.requestAction("window-close:" + windowId);
}

std::optional<WindowMinimizeState> UmbrielWorkspaceBackend::windowMinimizeState(std::string_view windowId) const {
  if (!m_runtime.available()) {
    return std::nullopt;
  }
  const auto it = std::ranges::find_if(m_windows, [&](const auto& window) { return window.first == windowId; });
  return it != m_windows.end() ? it->second.minimizeState : std::nullopt;
}

bool UmbrielWorkspaceBackend::minimizeWindowById(const std::string& windowId) {
  const auto state = windowMinimizeState(windowId);
  if (!state.has_value() || !state->minimizable || state->minimized) {
    return false;
  }
  return m_runtime.requestAction("window-minimize:" + windowId);
}

void UmbrielWorkspaceBackend::cleanup() { m_runtime.cleanup(); }

void UmbrielWorkspaceBackend::handleStreamReset() {
  const bool overviewWasOpen = m_overviewKnown && m_overviewOpen;
  m_windows.clear();
  m_workspacesByOutput.clear();
  m_focusedWindowId.reset();
  m_overviewKnown = false;
  m_overviewOpen = false;
  notifyChanged();
  notifyWindowsChanged();
  if (overviewWasOpen) {
    notifyOverviewChanged();
  }
}

void UmbrielWorkspaceBackend::handleEvent(std::string_view event, const nlohmann::json& data) {
  if (event == "windows") {
    if (!data.is_array()) {
      kLog.warn("windows snapshot must be an array");
      bool changed = false;
      for (auto& [windowId, window] : m_windows) {
        (void)windowId;
        changed |= window.minimizeState.has_value();
        window.minimizeState.reset();
      }
      if (changed) {
        notifyChanged();
        notifyWindowsChanged();
      }
      return;
    }

    std::vector<std::pair<std::string, Window>> next;
    next.reserve(data.size());
    std::optional<std::string> focused;
    for (const auto& entry : data) {
      if (!entry.is_object()) {
        continue;
      }
      const std::string id = jsonString(entry, "id");
      if (id.empty()) {
        continue;
      }
      Window window;
      window.workspaceId = jsonString(entry, "workspace");
      window.appId = jsonString(entry, "app_id");
      window.title = jsonString(entry, "title");
      window.active = entry.value("active", false);
      window.x = entry.value("x", 0);
      window.y = entry.value("y", 0);
      window.minimizeState = parseMinimizeState(entry);
      if (window.active && !focused.has_value()) {
        focused = id;
      }
      next.emplace_back(id, std::move(window));
    }

    m_focusedWindowId = std::move(focused);
    if (next == m_windows) {
      return;
    }
    m_windows = std::move(next);
    notifyChanged();
    notifyWindowsChanged();
    return;
  }

  if (event == "overview") {
    const bool open = data.value("open", false);
    const bool changed = !m_overviewKnown || m_overviewOpen != open;
    m_overviewKnown = true;
    m_overviewOpen = open;
    if (changed) {
      notifyOverviewChanged();
    }
    return;
  }

  // "keyboard_layout" belongs to the keyboard backend.
}

void UmbrielWorkspaceBackend::notifyChanged() const {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void UmbrielWorkspaceBackend::notifyWindowsChanged() const {
  if (m_windowChangeCallback) {
    m_windowChangeCallback();
  }
}

void UmbrielWorkspaceBackend::notifyOverviewChanged() const {
  if (m_overviewChangeCallback) {
    m_overviewChangeCallback();
  }
}
