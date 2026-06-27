#include "compositors/kde/kwin_workspace_backend.h"

#include "org-kde-plasma-virtual-desktop-client-protocol.h"

#include <algorithm>
#include <ranges>

namespace {

  constexpr std::uint32_t kManagementVersion = 4;

  [[nodiscard]] std::uint32_t proxyVersion(const void* proxy) {
    return wl_proxy_get_version(static_cast<wl_proxy*>(const_cast<void*>(proxy)));
  }

  void managementDesktopCreated(
      void* data, org_kde_plasma_virtual_desktop_management* /*management*/, const char* desktopId,
      std::uint32_t position
  ) {
    static_cast<KwinWorkspaceBackend*>(data)->onDesktopCreated(desktopId, position);
  }

  void managementDesktopRemoved(
      void* data, org_kde_plasma_virtual_desktop_management* /*management*/, const char* desktopId
  ) {
    static_cast<KwinWorkspaceBackend*>(data)->onDesktopRemoved(desktopId);
  }

  void managementDone(void* data, org_kde_plasma_virtual_desktop_management* /*management*/) {
    static_cast<KwinWorkspaceBackend*>(data)->onManagementDone();
  }

  void managementRows(void* data, org_kde_plasma_virtual_desktop_management* /*management*/, std::uint32_t rows) {
    static_cast<KwinWorkspaceBackend*>(data)->onManagementRows(rows);
  }

  const org_kde_plasma_virtual_desktop_management_listener kManagementListener = {
      .desktop_created = managementDesktopCreated,
      .desktop_removed = managementDesktopRemoved,
      .done = managementDone,
      .rows = managementRows,
  };

  void desktopDesktopId(void* data, org_kde_plasma_virtual_desktop* desktop, const char* id) {
    static_cast<KwinWorkspaceBackend*>(data)->onDesktopIdChanged(desktop, id);
  }

  void desktopName(void* data, org_kde_plasma_virtual_desktop* desktop, const char* name) {
    static_cast<KwinWorkspaceBackend*>(data)->onDesktopNameChanged(desktop, name);
  }

  void desktopActivated(void* data, org_kde_plasma_virtual_desktop* desktop) {
    static_cast<KwinWorkspaceBackend*>(data)->onDesktopActivated(desktop);
  }

  void desktopDeactivated(void* data, org_kde_plasma_virtual_desktop* desktop) {
    static_cast<KwinWorkspaceBackend*>(data)->onDesktopDeactivated(desktop);
  }

  void desktopDone(void* data, org_kde_plasma_virtual_desktop* desktop) {
    static_cast<KwinWorkspaceBackend*>(data)->onDesktopDone(desktop);
  }

  void desktopRemoved(void* data, org_kde_plasma_virtual_desktop* desktop) {
    static_cast<KwinWorkspaceBackend*>(data)->onDesktopRemovedHandle(desktop);
  }

  void desktopPosition(void* data, org_kde_plasma_virtual_desktop* desktop, std::uint32_t position) {
    static_cast<KwinWorkspaceBackend*>(data)->onDesktopPositionChanged(desktop, position);
  }

  void desktopOutputEntered(void* data, org_kde_plasma_virtual_desktop* desktop, const char* outputName) {
    static_cast<KwinWorkspaceBackend*>(data)->onDesktopOutputEntered(desktop, outputName);
  }

  const org_kde_plasma_virtual_desktop_listener kDesktopListener = {
      .desktop_id = desktopDesktopId,
      .name = desktopName,
      .activated = desktopActivated,
      .deactivated = desktopDeactivated,
      .done = desktopDone,
      .removed = desktopRemoved,
      .position = desktopPosition,
      .output_entered = desktopOutputEntered,
  };

} // namespace

void KwinWorkspaceBackend::bindKdeVirtualDesktop(org_kde_plasma_virtual_desktop_management* management) {
  m_management = management;
  m_managementVersion = proxyVersion(management);
  org_kde_plasma_virtual_desktop_management_add_listener(management, &kManagementListener, this);
}

void KwinWorkspaceBackend::setOutputNameResolver(OutputNameResolver resolver) {
  m_outputNameResolver = std::move(resolver);
}

void KwinWorkspaceBackend::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void KwinWorkspaceBackend::activate(const std::string& id) {
  const auto* desktop = findDesktopById(id);
  if (desktop == nullptr || desktop->handle == nullptr) {
    return;
  }
  org_kde_plasma_virtual_desktop_request_activate(desktop->handle);
}

void KwinWorkspaceBackend::activateForOutput(wl_output* output, const std::string& id) {
  const auto* desktop = findDesktopById(id);
  if (desktop == nullptr || desktop->handle == nullptr) {
    return;
  }

  if (output != nullptr && m_outputNameResolver) {
    const std::string outputName = m_outputNameResolver(output);
    if (!outputName.empty()
        && proxyVersion(desktop->handle) >= ORG_KDE_PLASMA_VIRTUAL_DESKTOP_REQUEST_ENTER_OUTPUT_SINCE_VERSION) {
      org_kde_plasma_virtual_desktop_request_enter_output(desktop->handle, outputName.c_str());
      return;
    }
  }

  activate(id);
}

void KwinWorkspaceBackend::activateForOutput(wl_output* output, const Workspace& workspace) {
  if (!workspace.id.empty()) {
    activateForOutput(output, workspace.id);
    return;
  }
  if (!workspace.name.empty()) {
    for (const auto& [handle, desktop] : m_desktops) {
      (void)handle;
      if (desktop.name == workspace.name) {
        activateForOutput(output, desktop.id);
        return;
      }
    }
  }
}

void KwinWorkspaceBackend::cleanup() {
  for (auto& [handle, _] : m_desktops) {
    if (handle != nullptr) {
      org_kde_plasma_virtual_desktop_destroy(handle);
    }
  }
  m_desktops.clear();

  if (m_management != nullptr) {
    org_kde_plasma_virtual_desktop_management_destroy(m_management);
    m_management = nullptr;
  }
  m_managementVersion = 0;
  m_rows = 1;
}

std::vector<Workspace> KwinWorkspaceBackend::all() const {
  std::vector<Workspace> result;
  result.reserve(m_desktops.size());
  for (const auto& [handle, desktop] : m_desktops) {
    (void)handle;
    if (desktop.id.empty()) {
      continue;
    }
    result.push_back(toWorkspace(desktop, {}));
  }
  std::ranges::sort(result, {}, &Workspace::coordinates);
  return result;
}

std::vector<Workspace> KwinWorkspaceBackend::forOutput(wl_output* output) const {
  std::string outputName;
  if (output != nullptr && m_outputNameResolver) {
    outputName = m_outputNameResolver(output);
  }

  std::vector<Workspace> result;
  result.reserve(m_desktops.size());
  for (const auto& [handle, desktop] : m_desktops) {
    (void)handle;
    if (desktop.id.empty()) {
      continue;
    }
    result.push_back(toWorkspace(desktop, outputName));
  }
  std::ranges::sort(result, {}, &Workspace::coordinates);
  return result;
}

void KwinWorkspaceBackend::onDesktopCreated(const char* desktopId, std::uint32_t position) {
  if (desktopId == nullptr || desktopId[0] == '\0' || m_management == nullptr) {
    return;
  }
  requestDesktopHandle(desktopId, position);
}

void KwinWorkspaceBackend::onDesktopRemoved(const char* desktopId) {
  if (desktopId == nullptr || desktopId[0] == '\0') {
    return;
  }
  removeDesktopById(desktopId);
}

void KwinWorkspaceBackend::onManagementDone() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void KwinWorkspaceBackend::onManagementRows(std::uint32_t rows) {
  if (rows == 0) {
    return;
  }
  m_rows = rows;
}

void KwinWorkspaceBackend::onDesktopIdChanged(org_kde_plasma_virtual_desktop* desktop, const char* id) {
  if (auto* state = findDesktop(desktop); state != nullptr) {
    state->id = id != nullptr ? id : "";
  }
}

void KwinWorkspaceBackend::onDesktopNameChanged(org_kde_plasma_virtual_desktop* desktop, const char* name) {
  if (auto* state = findDesktop(desktop); state != nullptr) {
    state->name = name != nullptr ? name : "";
  }
}

void KwinWorkspaceBackend::onDesktopActivated(org_kde_plasma_virtual_desktop* desktop) {
  if (auto* state = findDesktop(desktop); state != nullptr) {
    state->globallyActive = true;
    notifyChanged();
  }
}

void KwinWorkspaceBackend::onDesktopDeactivated(org_kde_plasma_virtual_desktop* desktop) {
  if (auto* state = findDesktop(desktop); state != nullptr) {
    state->globallyActive = false;
    notifyChanged();
  }
}

void KwinWorkspaceBackend::onDesktopPositionChanged(org_kde_plasma_virtual_desktop* desktop, std::uint32_t position) {
  if (auto* state = findDesktop(desktop); state != nullptr) {
    state->position = position;
  }
}

void KwinWorkspaceBackend::onDesktopOutputEntered(org_kde_plasma_virtual_desktop* desktop, const char* outputName) {
  if (outputName == nullptr || outputName[0] == '\0') {
    return;
  }

  const std::string output(outputName);
  for (auto& [handle, state] : m_desktops) {
    (void)handle;
    if (handle == desktop) {
      continue;
    }
    std::erase(state.activeOutputs, output);
  }

  if (auto* state = findDesktop(desktop); state != nullptr) {
    if (std::ranges::contains(state->activeOutputs, output)) {
      return;
    }
    state->activeOutputs.push_back(output);
    notifyChanged();
  }
}

void KwinWorkspaceBackend::onDesktopDone(org_kde_plasma_virtual_desktop* /*desktop*/) {}

void KwinWorkspaceBackend::onDesktopRemovedHandle(org_kde_plasma_virtual_desktop* desktop) {
  if (desktop == nullptr) {
    return;
  }
  const auto it = m_desktops.find(desktop);
  if (it == m_desktops.end()) {
    return;
  }
  org_kde_plasma_virtual_desktop_destroy(desktop);
  m_desktops.erase(it);
  notifyChanged();
}

void KwinWorkspaceBackend::requestDesktopHandle(const std::string& desktopId, std::uint32_t position) {
  if (findDesktopById(desktopId) != nullptr) {
    return;
  }

  org_kde_plasma_virtual_desktop* handle =
      org_kde_plasma_virtual_desktop_management_get_virtual_desktop(m_management, desktopId.c_str());
  if (handle == nullptr) {
    return;
  }

  DesktopState state{
      .handle = handle,
      .id = desktopId,
      .name = {},
      .position = position,
      .globallyActive = false,
      .activeOutputs = {},
  };
  m_desktops.emplace(handle, std::move(state));
  org_kde_plasma_virtual_desktop_add_listener(handle, &kDesktopListener, this);
}

void KwinWorkspaceBackend::removeDesktopById(const std::string& desktopId) {
  for (auto it = m_desktops.begin(); it != m_desktops.end(); ++it) {
    if (it->second.id != desktopId) {
      continue;
    }
    if (it->first != nullptr) {
      org_kde_plasma_virtual_desktop_destroy(it->first);
    }
    m_desktops.erase(it);
    notifyChanged();
    return;
  }
}

KwinWorkspaceBackend::DesktopState* KwinWorkspaceBackend::findDesktop(org_kde_plasma_virtual_desktop* desktop) {
  const auto it = m_desktops.find(desktop);
  return it != m_desktops.end() ? &it->second : nullptr;
}

const KwinWorkspaceBackend::DesktopState*
KwinWorkspaceBackend::findDesktop(org_kde_plasma_virtual_desktop* desktop) const {
  const auto it = m_desktops.find(desktop);
  return it != m_desktops.end() ? &it->second : nullptr;
}

KwinWorkspaceBackend::DesktopState* KwinWorkspaceBackend::findDesktopById(const std::string& id) {
  for (auto& [handle, desktop] : m_desktops) {
    (void)handle;
    if (desktop.id == id) {
      return &desktop;
    }
  }
  return nullptr;
}

const KwinWorkspaceBackend::DesktopState* KwinWorkspaceBackend::findDesktopById(const std::string& id) const {
  for (const auto& [handle, desktop] : m_desktops) {
    (void)handle;
    if (desktop.id == id) {
      return &desktop;
    }
  }
  return nullptr;
}

Workspace KwinWorkspaceBackend::toWorkspace(const DesktopState& desktop, const std::string& outputName) const {
  Workspace workspace;
  workspace.id = desktop.id;
  workspace.name = desktop.name.empty() ? ("Desktop " + std::to_string(desktop.position + 1)) : desktop.name;
  workspace.index = desktop.position + 1;

  const auto columns = gridColumns();
  if (columns > 0) {
    workspace.coordinates = {desktop.position % columns, desktop.position / columns};
  }

  if (!outputName.empty() && !desktop.activeOutputs.empty()) {
    workspace.active = std::ranges::contains(desktop.activeOutputs, outputName);
  } else {
    workspace.active = desktop.globallyActive;
  }

  return workspace;
}

std::uint32_t KwinWorkspaceBackend::gridColumns() const {
  if (m_desktops.empty() || m_rows == 0) {
    return 1;
  }
  return (static_cast<std::uint32_t>(m_desktops.size()) + m_rows - 1) / m_rows;
}

void KwinWorkspaceBackend::notifyChanged() const {
  if (m_changeCallback) {
    m_changeCallback();
  }
}
