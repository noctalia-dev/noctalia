#include "compositors/sway/sway_workspace_backend.h"

#include "compositors/sway/sway_runtime.h"
#include "core/log.h"
#include "util/string_utils.h"

#include <algorithm>
#include <format>
#include <nlohmann/json.hpp>
#include <string_view>
#include <unordered_set>

namespace {

  namespace ipc = compositors::sway::ipc;

  constexpr Logger kLog("workspace_sway");

  std::string jsonStringValue(const nlohmann::json& object, std::string_view key) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null() || !it->is_string()) {
      return {};
    }
    return it->get<std::string>();
  }

  void tallyWorkspaceWindows(
      const nlohmann::json& node, const std::string& currentWorkspace,
      std::unordered_map<std::string, std::size_t>& counts
  ) {
    if (!node.is_object()) {
      return;
    }

    std::string workspaceName = currentWorkspace;
    if (const auto typeIt = node.find("type");
        typeIt != node.end() && typeIt->is_string() && typeIt->get<std::string>() == "workspace") {
      if (const std::string name = jsonStringValue(node, "name"); !name.empty()) {
        workspaceName = name;
      }
    }

    const auto nodesIt = node.find("nodes");
    const auto floatingIt = node.find("floating_nodes");

    const bool hasNodes = nodesIt != node.end() && nodesIt->is_array();
    const bool hasFloating = floatingIt != node.end() && floatingIt->is_array();
    const bool isLeaf = (!hasNodes || nodesIt->empty()) && (!hasFloating || floatingIt->empty());

    bool hasWindow = false;
    if (const auto windowIt = node.find("window"); windowIt != node.end() && !windowIt->is_null()) {
      hasWindow = true;
    } else if (const auto appIdIt = node.find("app_id"); appIdIt != node.end() && appIdIt->is_string()) {
      hasWindow = !appIdIt->get<std::string>().empty();
    }

    if (isLeaf && hasWindow && !workspaceName.empty()) {
      ++counts[workspaceName];
    }

    if (hasNodes) {
      for (const auto& child : *nodesIt) {
        tallyWorkspaceWindows(child, workspaceName, counts);
      }
    }
    if (hasFloating) {
      for (const auto& child : *floatingIt) {
        tallyWorkspaceWindows(child, workspaceName, counts);
      }
    }
  }

  void collectWorkspaceApps(
      const nlohmann::json& node, const std::string& currentWorkspace,
      std::unordered_map<std::string, std::vector<std::string>>& appIdsByWorkspace,
      std::unordered_map<std::string, std::unordered_set<std::string>>& seenPerWorkspace
  ) {
    if (!node.is_object()) {
      return;
    }

    std::string workspaceName = currentWorkspace;
    if (const auto typeIt = node.find("type");
        typeIt != node.end() && typeIt->is_string() && typeIt->get<std::string>() == "workspace") {
      if (const std::string name = jsonStringValue(node, "name"); !name.empty()) {
        workspaceName = name;
      }
    }

    std::string appId;
    if (const auto appIdIt = node.find("app_id"); appIdIt != node.end() && appIdIt->is_string()) {
      appId = appIdIt->get<std::string>();
    }
    if (appId.empty()) {
      if (const auto propsIt = node.find("window_properties"); propsIt != node.end() && propsIt->is_object()) {
        const auto classIt = propsIt->find("class");
        if (classIt != propsIt->end() && classIt->is_string()) {
          appId = classIt->get<std::string>();
        }
      }
    }
    if (!workspaceName.empty() && !appId.empty()) {
      auto& seen = seenPerWorkspace[workspaceName];
      if (seen.insert(appId).second) {
        appIdsByWorkspace[workspaceName].push_back(appId);
      }
    }

    const auto nodesIt = node.find("nodes");
    if (nodesIt != node.end() && nodesIt->is_array()) {
      for (const auto& child : *nodesIt) {
        collectWorkspaceApps(child, workspaceName, appIdsByWorkspace, seenPerWorkspace);
      }
    }
    const auto floatingIt = node.find("floating_nodes");
    if (floatingIt != node.end() && floatingIt->is_array()) {
      for (const auto& child : *floatingIt) {
        collectWorkspaceApps(child, workspaceName, appIdsByWorkspace, seenPerWorkspace);
      }
    }
  }

  void collectWorkspaceWindows(
      const nlohmann::json& node, const std::string& currentWorkspace, const std::string& currentWorkspaceKey,
      std::vector<WorkspaceWindow>& windows, std::string& focusedExtToplevelIdentifier,
      bool& publishesExtToplevelIdentity
  ) {
    if (!node.is_object()) {
      return;
    }

    std::string workspaceName = currentWorkspace;
    std::string workspaceKey = currentWorkspaceKey;
    if (const auto typeIt = node.find("type");
        typeIt != node.end() && typeIt->is_string() && typeIt->get<std::string>() == "workspace") {
      if (const std::string name = jsonStringValue(node, "name"); !name.empty()) {
        workspaceName = name;
      }
      if (const auto numIt = node.find("num"); numIt != node.end() && numIt->is_number_integer()) {
        const int num = numIt->get<int>();
        workspaceKey = num > 0 ? std::to_string(num) : workspaceName;
      } else {
        workspaceKey = workspaceName;
      }
    }

    std::string appId;
    if (const auto appIdIt = node.find("app_id"); appIdIt != node.end() && appIdIt->is_string()) {
      appId = appIdIt->get<std::string>();
    }
    if (appId.empty()) {
      if (const auto propsIt = node.find("window_properties"); propsIt != node.end() && propsIt->is_object()) {
        const auto classIt = propsIt->find("class");
        if (classIt != propsIt->end() && classIt->is_string()) {
          appId = classIt->get<std::string>();
        }
      }
    }

    const auto nodesIt = node.find("nodes");
    const auto floatingIt = node.find("floating_nodes");
    const bool hasNodes = nodesIt != node.end() && nodesIt->is_array();
    const bool hasFloating = floatingIt != node.end() && floatingIt->is_array();
    const bool isLeaf = (!hasNodes || nodesIt->empty()) && (!hasFloating || floatingIt->empty());
    std::string swayNodeId;
    if (const auto idIt = node.find("id"); idIt != node.end() && idIt->is_number_integer()) {
      swayNodeId = std::to_string(idIt->get<std::int64_t>());
    }
    // Present only on nodes holding a view, and independent of app_id, so XWayland views are
    // covered even though sway reports no app_id for them.
    const std::string extIdentifier = jsonStringValue(node, "foreign_toplevel_identifier");
    if (!extIdentifier.empty()) {
      publishesExtToplevelIdentity = true;
      if (node.value("focused", false)) {
        focusedExtToplevelIdentifier = extIdentifier;
      }
    }
    std::int32_t x = 0;
    std::int32_t y = 0;
    if (const auto rectIt = node.find("rect"); rectIt != node.end() && rectIt->is_object()) {
      x = rectIt->value("x", 0);
      y = rectIt->value("y", 0);
    }

    if (isLeaf && !workspaceName.empty() && !workspaceKey.empty() && !appId.empty()) {
      windows.push_back(
          WorkspaceWindow{
              .windowId = swayNodeId,
              .workspaceKey = workspaceKey,
              .appId = appId,
              .title = StringUtils::windowTitleSingleLine(jsonStringValue(node, "name")),
              .x = x,
              .y = y,
              .outputName = {},
          }
      );
    }

    if (hasNodes) {
      for (const auto& child : *nodesIt) {
        collectWorkspaceWindows(
            child, workspaceName, workspaceKey, windows, focusedExtToplevelIdentifier, publishesExtToplevelIdentity
        );
      }
    }
    if (hasFloating) {
      for (const auto& child : *floatingIt) {
        collectWorkspaceWindows(
            child, workspaceName, workspaceKey, windows, focusedExtToplevelIdentifier, publishesExtToplevelIdentity
        );
      }
    }
  }

  std::string assignmentLookupKey(std::string_view workspaceKey, std::string_view appId, std::string_view title) {
    std::string key;
    key.reserve(workspaceKey.size() + appId.size() + title.size() + 2);
    key.append(workspaceKey);
    key.push_back('\n');
    key.append(appId);
    key.push_back('\n');
    key.append(title);
    return key;
  }

} // namespace

SwayWorkspaceBackend::SwayWorkspaceBackend(
    OutputNameResolver outputNameResolver, compositors::sway::SwayRuntime& runtime
)
    : compositors::sway::SwayEventHandler(runtime), m_outputNameResolver(std::move(outputNameResolver)) {}

void SwayWorkspaceBackend::setOutputNameResolver(OutputNameResolver outputNameResolver) {
  m_outputNameResolver = std::move(outputNameResolver);
}

bool SwayWorkspaceBackend::connectSocket() {
  if (!m_runtime.connectSocket()) {
    return false;
  }
  requestSnapshot();
  return true;
}

bool SwayWorkspaceBackend::isAvailable() const noexcept { return m_runtime.connected(); }

void SwayWorkspaceBackend::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void SwayWorkspaceBackend::activate(const std::string& id) {
  if (!m_runtime.connected() || id.empty()) {
    return;
  }

  m_runtime.sendMessage(ipc::kRunCommand, "workspace " + StringUtils::quoteDouble(id));
}

void SwayWorkspaceBackend::activateForOutput(wl_output* /*output*/, const std::string& id) { activate(id); }

void SwayWorkspaceBackend::activateForOutput(wl_output* /*output*/, const Workspace& workspace) {
  activate(workspace.id.empty() ? workspace.name : workspace.id);
}

std::vector<Workspace> SwayWorkspaceBackend::all() const {
  std::vector<Workspace> result;
  result.reserve(m_workspaces.size());
  for (const auto& workspace : m_workspaces) {
    result.push_back(toWorkspace(workspace));
  }
  return result;
}

std::vector<Workspace> SwayWorkspaceBackend::forOutput(wl_output* output) const {
  const std::string outputName = m_outputNameResolver != nullptr ? m_outputNameResolver(output) : std::string{};
  if (outputName.empty()) {
    return {};
  }

  std::vector<Workspace> result;
  for (const auto& workspace : m_workspaces) {
    if (workspace.output == outputName) {
      result.push_back(toWorkspace(workspace));
    }
  }
  return result;
}

std::unordered_map<std::string, std::vector<std::string>>
SwayWorkspaceBackend::appIdsByWorkspace(wl_output* output) const {
  const std::string outputName = m_outputNameResolver != nullptr ? m_outputNameResolver(output) : std::string{};
  if (output != nullptr && outputName.empty()) {
    return {};
  }
  if (outputName.empty()) {
    return m_workspaceApps;
  }

  std::unordered_set<std::string> workspaceNames;
  for (const auto& workspace : m_workspaces) {
    if (workspace.output == outputName) {
      workspaceNames.insert(workspace.name);
    }
  }

  std::unordered_map<std::string, std::vector<std::string>> filtered;
  for (const auto& [workspace, apps] : m_workspaceApps) {
    if (workspaceNames.contains(workspace)) {
      filtered.emplace(workspace, apps);
    }
  }
  return filtered;
}

std::unordered_map<std::uintptr_t, WorkspaceWindow> SwayWorkspaceBackend::assignTaskbarWindows(
    const std::vector<TaskbarWindowCandidate>& windows, wl_output* output
) const {
  const std::string outputName = m_outputNameResolver != nullptr ? m_outputNameResolver(output) : std::string{};
  if (output != nullptr && outputName.empty()) {
    return {};
  }

  std::vector<const SwayWorkspace*> orderedWorkspaces;
  orderedWorkspaces.reserve(m_workspaces.size());
  for (const auto& workspace : m_workspaces) {
    if (!outputName.empty() && workspace.output != outputName) {
      continue;
    }
    orderedWorkspaces.push_back(&workspace);
  }

  std::vector<WorkspaceWindow> outputWindows = workspaceWindows(output);
  std::unordered_map<std::string, std::vector<const WorkspaceWindow*>> windowsByLookupKey;
  windowsByLookupKey.reserve(outputWindows.size());
  for (const auto& window : outputWindows) {
    const std::string appIdLower = StringUtils::toLower(window.appId);
    windowsByLookupKey[assignmentLookupKey(window.workspaceKey, appIdLower, window.title)].push_back(&window);
  }

  std::unordered_map<std::string, std::size_t> usageCounts;
  std::unordered_map<std::uintptr_t, WorkspaceWindow> assigned;
  assigned.reserve(windows.size());

  for (const auto& window : windows) {
    if (window.handleKey == 0 || window.appIds.empty()) {
      continue;
    }

    bool matched = false;
    for (const SwayWorkspace* workspace : orderedWorkspaces) {
      if (workspace == nullptr) {
        continue;
      }
      const std::string workspaceKey = workspace->num > 0 ? std::to_string(workspace->num) : workspace->name;
      if (workspaceKey.empty()) {
        continue;
      }

      for (const auto& candidateAppId : window.appIds) {
        const std::string lookupKey =
            assignmentLookupKey(workspaceKey, StringUtils::toLower(candidateAppId), window.title);
        auto it = windowsByLookupKey.find(lookupKey);
        if (it == windowsByLookupKey.end()) {
          continue;
        }
        std::size_t& occurrence = usageCounts[lookupKey];
        if (occurrence >= it->second.size()) {
          continue;
        }
        assigned.emplace(window.handleKey, *it->second[occurrence]);
        ++occurrence;
        matched = true;
        break;
      }

      if (matched) {
        break;
      }
    }
  }

  return assigned;
}

std::vector<WorkspaceWindow> SwayWorkspaceBackend::workspaceWindows(wl_output* output) const {
  const std::string outputName = m_outputNameResolver != nullptr ? m_outputNameResolver(output) : std::string{};
  if (output != nullptr && outputName.empty()) {
    return {};
  }
  if (outputName.empty()) {
    return m_workspaceWindows;
  }

  std::unordered_set<std::string> workspaceKeys;
  for (const auto& workspace : m_workspaces) {
    if (workspace.output != outputName) {
      continue;
    }
    if (workspace.num > 0) {
      workspaceKeys.insert(std::to_string(workspace.num));
    } else if (!workspace.name.empty()) {
      workspaceKeys.insert(workspace.name);
    }
  }

  std::vector<WorkspaceWindow> filtered;
  for (const auto& window : m_workspaceWindows) {
    if (workspaceKeys.contains(window.workspaceKey)) {
      filtered.push_back(window);
    }
  }
  return filtered;
}

void SwayWorkspaceBackend::focusWindow(const std::string& windowId) {
  if (!m_runtime.connected() || windowId.empty()) {
    return;
  }

  m_runtime.sendMessage(ipc::kRunCommand, "[con_id=" + windowId + "] focus");
}

void SwayWorkspaceBackend::cleanup() { m_runtime.cleanup(); }

int SwayWorkspaceBackend::pollFd() const noexcept { return m_runtime.pollFd(); }

void SwayWorkspaceBackend::dispatchPoll(short revents) { m_runtime.dispatchPoll(revents); }

void SwayWorkspaceBackend::handleCleanup() {
  m_workspaces.clear();
  m_workspaceApps.clear();
  m_workspaceWindows.clear();
  m_focusedExtToplevelIdentifier.clear();
}

void SwayWorkspaceBackend::handleDisconnect() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void SwayWorkspaceBackend::requestSnapshot() {
  m_runtime.sendMessage(ipc::kGetWorkspaces, "");
  m_runtime.sendMessage(ipc::kGetTree, "");
}

void SwayWorkspaceBackend::requestTree() { m_runtime.sendMessage(ipc::kGetTree, ""); }

void SwayWorkspaceBackend::handleMessage(std::uint32_t type, std::string_view payload) {
  if (type == ipc::kGetWorkspaces) {
    parseWorkspaceList(payload);
    return;
  }
  if (type == ipc::kGetTree) {
    parseTree(payload);
    return;
  }
  if (type == ipc::kWorkspaceEvent) {
    refreshFromWorkspaceEvent();
    return;
  }
  if (type == ipc::kWindowEvent) {
    requestTree();
  }
}

void SwayWorkspaceBackend::parseWorkspaceList(std::string_view payload) {
  try {
    const auto json = nlohmann::json::parse(payload);
    if (!json.is_array()) {
      return;
    }

    std::vector<SwayWorkspace> next;
    next.reserve(json.size());
    std::size_t ordinal = 0;
    for (const auto& item : json) {
      if (!item.is_object()) {
        continue;
      }
      SwayWorkspace workspace;
      workspace.name = jsonStringValue(item, "name");
      workspace.output = jsonStringValue(item, "output");
      workspace.visible = item.value("visible", false);
      workspace.urgent = item.value("urgent", false);
      workspace.num = item.value("num", -1);
      workspace.ordinal = ordinal++;
      auto occupancy = m_workspaceOccupancy.find(workspace.name);
      workspace.occupied = occupancy != m_workspaceOccupancy.end() && occupancy->second > 0;
      if (!workspace.name.empty()) {
        next.push_back(std::move(workspace));
      }
    }

    std::ranges::sort(next, [](const auto& a, const auto& b) {
      if ((a.num >= 0) != (b.num >= 0)) {
        return a.num >= 0;
      }
      if (a.num >= 0 && b.num >= 0 && a.num != b.num) {
        return a.num < b.num;
      }
      return a.ordinal < b.ordinal;
    });

    m_workspaces = std::move(next);
    if (m_changeCallback) {
      m_changeCallback();
    }
  } catch (const nlohmann::json::exception& e) {
    kLog.warn("failed to parse sway workspaces: {}", e.what());
  }
}

void SwayWorkspaceBackend::parseTree(std::string_view payload) {
  try {
    const auto json = nlohmann::json::parse(payload);
    if (!json.is_object()) {
      return;
    }

    std::unordered_map<std::string, std::size_t> occupancy;
    tallyWorkspaceWindows(json, std::string{}, occupancy);
    m_workspaceOccupancy = std::move(occupancy);
    std::unordered_map<std::string, std::vector<std::string>> appIdsByWorkspace;
    std::unordered_map<std::string, std::unordered_set<std::string>> seenPerWorkspace;
    collectWorkspaceApps(json, std::string{}, appIdsByWorkspace, seenPerWorkspace);
    m_workspaceApps = std::move(appIdsByWorkspace);
    std::vector<WorkspaceWindow> workspaceWindows;
    std::string focusedExtToplevelIdentifier;
    collectWorkspaceWindows(
        json, std::string{}, std::string{}, workspaceWindows, focusedExtToplevelIdentifier,
        m_publishesExtToplevelIdentity
    );
    m_workspaceWindows = std::move(workspaceWindows);
    m_focusedExtToplevelIdentifier = std::move(focusedExtToplevelIdentifier);

    if (!m_workspaces.empty()) {
      for (auto& workspace : m_workspaces) {
        auto it = m_workspaceOccupancy.find(workspace.name);
        workspace.occupied = it != m_workspaceOccupancy.end() && it->second > 0;
      }
      if (m_changeCallback) {
        m_changeCallback();
      }
    }
  } catch (const nlohmann::json::exception& e) {
    kLog.warn("failed to parse sway tree: {}", e.what());
  }
}

void SwayWorkspaceBackend::refreshFromWorkspaceEvent() { requestSnapshot(); }

Workspace SwayWorkspaceBackend::toWorkspace(const SwayWorkspace& workspace) {
  const std::uint32_t coord = workspace.num >= 0 ? static_cast<std::uint32_t>(workspace.num - 1)
                                                 : static_cast<std::uint32_t>(workspace.ordinal);
  return Workspace{
      .id = workspace.name,
      .name = workspace.name,
      .coordinates = {coord},
      .index = workspace.num > 0 ? static_cast<std::uint32_t>(workspace.num) : 0,
      .active = workspace.visible,
      .urgent = workspace.urgent,
      .occupied = workspace.occupied,
  };
}
