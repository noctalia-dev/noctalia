#include "wayland/wayland_workspaces.h"

#include "compositors/compositor_detect.h"
#include "compositors/compositor_runtime.h"
#include "compositors/ext_workspace/ext_workspace_backend.h"
#include "compositors/hyprland/hyprland_workspace_backend.h"
#include "compositors/mango/mango_workspace_backend.h"
#include "compositors/output_backend.h"
#include "compositors/sway/sway_workspace_backend.h"
#include "compositors/triad/triad_workspace_backend.h"
#include "core/log.h"

#include <string>

namespace {

  constexpr Logger kLog("workspace");

} // namespace

WaylandWorkspaces::WaylandWorkspaces(compositors::CompositorRuntimeRegistry& runtimeRegistry) {
  auto extBackend = std::make_unique<ExtWorkspaceBackend>();
  m_extWorkspaceBinder = extBackend.get();
  m_extBackend = extBackend.get();
  m_backends.push_back(std::move(extBackend));

  auto dwlIpcBackend = std::make_unique<MangoWorkspaceBackend>();
  m_dwlIpcWorkspaceBinder = dwlIpcBackend.get();
  m_dwlIpcBackend = dwlIpcBackend.get();
  m_outputObservers.push_back(dwlIpcBackend.get());
  m_backends.push_back(std::move(dwlIpcBackend));

  auto hyprlandBackend = std::make_unique<HyprlandWorkspaceBackend>(
      [](wl_output* /*output*/) { return std::string{}; }, runtimeRegistry.hyprland()
  );
  m_hyprlandBackend = hyprlandBackend.get();
  m_hyprlandConnector = hyprlandBackend.get();
  m_outputNameResolvers.push_back(hyprlandBackend.get());
  m_backends.push_back(std::move(hyprlandBackend));

  auto swayBackend = std::make_unique<SwayWorkspaceBackend>(
      [](wl_output* /*output*/) { return std::string{}; }, runtimeRegistry.sway()
  );
  m_swayBackend = swayBackend.get();
  m_swayConnector = swayBackend.get();
  m_outputNameResolvers.push_back(swayBackend.get());
  m_backends.push_back(std::move(swayBackend));

  auto triadBackend = std::make_unique<TriadWorkspaceBackend>(runtimeRegistry.triad());
  m_triadBackend = triadBackend.get();
  m_triadConnector = triadBackend.get();
  m_outputNameResolvers.push_back(triadBackend.get());
  m_backends.push_back(std::move(triadBackend));
}

WaylandWorkspaces::~WaylandWorkspaces() = default;

void WaylandWorkspaces::bindExtWorkspace(ext_workspace_manager_v1* manager) {
  if (m_extWorkspaceBinder != nullptr) {
    m_extWorkspaceBinder->bindExtWorkspace(manager);
  }
}

void WaylandWorkspaces::bindDwlIpcWorkspace(zdwl_ipc_manager_v2* manager) {
  if (m_dwlIpcWorkspaceBinder != nullptr) {
    m_dwlIpcWorkspaceBinder->bindDwlIpcWorkspace(manager);
  }
}

void WaylandWorkspaces::setOutputNameResolver(std::function<std::string(wl_output*)> resolver) {
  for (auto* backend : m_outputNameResolvers) {
    if (backend != nullptr) {
      backend->setOutputNameResolver(resolver);
    }
  }
}

void WaylandWorkspaces::initialize() {
  auto availableOrConnected = [](WorkspaceBackend* backend, WorkspaceSocketConnector* connector = nullptr) {
    return backend != nullptr && (backend->isAvailable() || (connector != nullptr && connector->connectSocket()));
  };

  switch (compositors::detect()) {
  case compositors::CompositorKind::Hyprland:
    if (availableOrConnected(m_hyprlandBackend, m_hyprlandConnector)) {
      setActiveBackend(m_hyprlandBackend);
      return;
    }
    break;
  case compositors::CompositorKind::Mango:
    if (availableOrConnected(m_dwlIpcBackend)) {
      setActiveBackend(m_dwlIpcBackend);
      return;
    }
    break;
  case compositors::CompositorKind::Sway:
    if (availableOrConnected(m_swayBackend, m_swayConnector)) {
      setActiveBackend(m_swayBackend);
      return;
    }
    break;
  case compositors::CompositorKind::Triad:
    if (m_triadBackend != nullptr && m_triadConnector != nullptr &&
        (m_triadConnector->connectSocket() || m_triadBackend->isAvailable())) {
      setActiveBackend(m_triadBackend);
      return;
    }
    break;
  case compositors::CompositorKind::Niri:
  case compositors::CompositorKind::Labwc:
  case compositors::CompositorKind::Unknown:
    break;
  }

  if (availableOrConnected(m_extBackend)) {
    setActiveBackend(m_extBackend);
    return;
  }
  if (availableOrConnected(m_hyprlandBackend, m_hyprlandConnector)) {
    setActiveBackend(m_hyprlandBackend);
    return;
  }
  if (availableOrConnected(m_dwlIpcBackend)) {
    setActiveBackend(m_dwlIpcBackend);
    return;
  }
  if (availableOrConnected(m_swayBackend, m_swayConnector)) {
    setActiveBackend(m_swayBackend);
    return;
  }
  if (m_triadBackend != nullptr && m_triadConnector != nullptr &&
      (m_triadConnector->connectSocket() || m_triadBackend->isAvailable())) {
    setActiveBackend(m_triadBackend);
    return;
  }

  setActiveBackend(nullptr);
}

void WaylandWorkspaces::onOutputAdded(wl_output* output) {
  for (auto* backend : m_outputObservers) {
    if (backend != nullptr) {
      backend->onOutputAdded(output);
    }
  }
  if (m_activeBackend == m_hyprlandBackend && m_hyprlandBackend != nullptr) {
    static_cast<HyprlandWorkspaceBackend*>(m_hyprlandBackend)->syncFromCompositor();
  }
}

void WaylandWorkspaces::onOutputRemoved(wl_output* output) {
  for (auto* backend : m_outputObservers) {
    if (backend != nullptr) {
      backend->onOutputRemoved(output);
    }
  }
}

void WaylandWorkspaces::setChangeCallback(ChangeCallback callback) {
  m_changeCallback = std::move(callback);
  auto wrapper = [this]() { notifyChanged(); };
  for (const auto& backend : m_backends) {
    if (backend != nullptr) {
      backend->setChangeCallback(wrapper);
    }
  }
}

void WaylandWorkspaces::activate(const std::string& id) {
  if (m_activeBackend != nullptr) {
    m_activeBackend->activate(id);
  }
}

void WaylandWorkspaces::activateForOutput(wl_output* output, const std::string& id) {
  if (m_activeBackend != nullptr) {
    m_activeBackend->activateForOutput(output, id);
  }
}

void WaylandWorkspaces::activateForOutput(wl_output* output, const Workspace& workspace) {
  if (m_activeBackend != nullptr) {
    m_activeBackend->activateForOutput(output, workspace);
  }
}

void WaylandWorkspaces::cleanup() {
  for (const auto& backend : m_backends) {
    if (backend != nullptr) {
      backend->cleanup();
    }
  }
  m_activeBackend = nullptr;
}

int WaylandWorkspaces::pollFd() const noexcept { return m_activeBackend != nullptr ? m_activeBackend->pollFd() : -1; }

short WaylandWorkspaces::pollEvents() const noexcept {
  return m_activeBackend != nullptr ? m_activeBackend->pollEvents() : static_cast<short>(POLLIN);
}

int WaylandWorkspaces::pollTimeoutMs() const noexcept {
  return m_activeBackend != nullptr ? m_activeBackend->pollTimeoutMs() : -1;
}

void WaylandWorkspaces::dispatchPoll(short revents) {
  if (m_activeBackend != nullptr) {
    m_activeBackend->dispatchPoll(revents);
  }
}

const char* WaylandWorkspaces::backendName() const noexcept {
  return m_activeBackend != nullptr ? m_activeBackend->backendName() : "none";
}

std::unordered_map<std::string, std::vector<std::string>>
WaylandWorkspaces::appIdsByWorkspace(wl_output* output) const {
  return m_activeBackend != nullptr ? m_activeBackend->appIdsByWorkspace(output)
                                    : std::unordered_map<std::string, std::vector<std::string>>{};
}

TaskbarAssignmentMode WaylandWorkspaces::taskbarAssignmentMode() const noexcept {
  return m_activeBackend != nullptr ? m_activeBackend->taskbarAssignmentMode() : TaskbarAssignmentMode::Generic;
}

std::unordered_map<std::uintptr_t, WorkspaceWindow>
WaylandWorkspaces::assignTaskbarWindows(const std::vector<TaskbarWindowCandidate>& windows, wl_output* output) const {
  return m_activeBackend != nullptr ? m_activeBackend->assignTaskbarWindows(windows, output)
                                    : std::unordered_map<std::uintptr_t, WorkspaceWindow>{};
}

std::vector<WorkspaceWindow> WaylandWorkspaces::workspaceWindows(wl_output* output) const {
  return m_activeBackend != nullptr ? m_activeBackend->workspaceWindows(output) : std::vector<WorkspaceWindow>{};
}

void WaylandWorkspaces::focusWindow(const std::string& windowId) const {
  if (m_activeBackend != nullptr) {
    m_activeBackend->focusWindow(windowId);
  }
}

std::optional<std::string> WaylandWorkspaces::focusedWindowId() const {
  if (m_triadBackend != nullptr && m_activeBackend == m_triadBackend) {
    return static_cast<const TriadWorkspaceBackend*>(m_triadBackend)->focusedWindowId();
  }
  if (m_hyprlandBackend != nullptr) {
    return static_cast<const HyprlandWorkspaceBackend*>(m_hyprlandBackend)->focusedWindowId();
  }
  return std::nullopt;
}

std::vector<Workspace> WaylandWorkspaces::all() const {
  return m_activeBackend != nullptr ? m_activeBackend->all() : std::vector<Workspace>{};
}

std::vector<Workspace> WaylandWorkspaces::forOutput(wl_output* output) const {
  return m_activeBackend != nullptr ? m_activeBackend->forOutput(output) : std::vector<Workspace>{};
}

wl_output* WaylandWorkspaces::dwlIpcSelectedOutput() const {
  if (m_dwlIpcBackend == nullptr || !m_dwlIpcBackend->isAvailable()) {
    return nullptr;
  }
  return static_cast<MangoWorkspaceBackend*>(m_dwlIpcBackend)->ipcSelectedOutput();
}

std::optional<std::pair<std::string, std::string>>
WaylandWorkspaces::dwlIpcFocusedClientOnOutput(wl_output* output) const {
  if (m_dwlIpcBackend == nullptr || !m_dwlIpcBackend->isAvailable()) {
    return std::nullopt;
  }
  return static_cast<const MangoWorkspaceBackend*>(m_dwlIpcBackend)->ipcFocusedClientForOutput(output);
}

void WaylandWorkspaces::setActiveBackend(WorkspaceBackend* backend) {
  m_activeBackend = backend;
  kLog.info("workspace backend={}", backendName());
}

void WaylandWorkspaces::notifyChanged() const {
  if (m_changeCallback) {
    m_changeCallback();
  }
}
