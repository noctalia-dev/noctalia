#include "wayland/wayland_workspaces.h"

#include "compositors/compositor_detect.h"
#include "compositors/compositor_runtime.h"
#include "compositors/dwl/dwl_workspace_backend.h"
#include "compositors/ext_workspace/ext_workspace_backend.h"
#include "compositors/hyprland/hyprland_workspace_backend.h"
#include "compositors/kde/kwin_workspace_backend.h"
#include "compositors/mango/mango_workspace_backend.h"
#include "compositors/output_backend.h"
#include "compositors/sway/sway_workspace_backend.h"
#include "compositors/triad/triad_workspace_backend.h"
#include "core/log.h"
#include "dwl-ipc-unstable-v2-client-protocol.h"

#include <string>

namespace {

  constexpr Logger kLog("workspace");

} // namespace

WaylandWorkspaces::WaylandWorkspaces(compositors::CompositorRuntimeRegistry& runtimeRegistry) {
  auto extBackend = std::make_unique<ExtWorkspaceBackend>();
  m_extWorkspaceBinder = extBackend.get();
  m_extBackend = extBackend.get();
  m_backends.push_back(std::move(extBackend));

  auto mangoIpcBackend = std::make_unique<MangoWorkspaceBackend>(runtimeRegistry.mango());
  m_mangoIpcBackend = mangoIpcBackend.get();
  m_mangoIpcConnector = mangoIpcBackend.get();
  m_outputNameResolvers.push_back(mangoIpcBackend.get());
  m_outputObservers.push_back(mangoIpcBackend.get());
  m_backends.push_back(std::move(mangoIpcBackend));

  auto dwlIpcBackend = std::make_unique<DwlWorkspaceBackend>();
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

  auto kwinBackend = std::make_unique<KwinWorkspaceBackend>();
  m_kwinBackend = kwinBackend.get();
  m_kdeVirtualDesktopBinder = kwinBackend.get();
  m_outputNameResolvers.push_back(kwinBackend.get());
  m_backends.push_back(std::move(kwinBackend));
}

WaylandWorkspaces::~WaylandWorkspaces() = default;

void WaylandWorkspaces::bindExtWorkspace(ext_workspace_manager_v1* manager) {
  if (m_extWorkspaceBinder != nullptr) {
    m_extWorkspaceBinder->bindExtWorkspace(manager);
  }
}

void WaylandWorkspaces::bindKdeVirtualDesktop(org_kde_plasma_virtual_desktop_management* management) {
  if (m_kdeVirtualDesktopBinder != nullptr) {
    m_kdeVirtualDesktopBinder->bindKdeVirtualDesktop(management);
  }
}

void WaylandWorkspaces::bindDwlIpcWorkspace(zdwl_ipc_manager_v2* manager) {
  if (manager == nullptr) {
    return;
  }
  // Mango exposes dwl-ipc for legacy clients; workspace state comes from Mango socket IPC only.
  if (compositors::detect() == compositors::CompositorKind::Mango) {
    if (m_dwlIpcBackend != nullptr) {
      m_dwlIpcBackend->cleanup();
    }
    zdwl_ipc_manager_v2_release(manager);
    return;
  }
  if (m_dwlIpcWorkspaceBinder != nullptr) {
    m_dwlIpcWorkspaceBinder->bindDwlIpcWorkspace(manager);
  } else {
    zdwl_ipc_manager_v2_release(manager);
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
  auto tryTriad = [&]() {
    return m_triadBackend != nullptr
        && m_triadConnector != nullptr
        && (m_triadConnector->connectSocket() || m_triadBackend->isAvailable());
  };
  auto tryFallback = [&](bool includeMangoIpc, bool includeDwlIpc) {
    if (availableOrConnected(m_extBackend)) {
      setActiveBackend(m_extBackend);
      return true;
    }
    if (availableOrConnected(m_hyprlandBackend, m_hyprlandConnector)) {
      setActiveBackend(m_hyprlandBackend);
      return true;
    }
    if (includeMangoIpc && availableOrConnected(m_mangoIpcBackend, m_mangoIpcConnector)) {
      setActiveBackend(m_mangoIpcBackend);
      return true;
    }
    if (includeDwlIpc && availableOrConnected(m_dwlIpcBackend)) {
      setActiveBackend(m_dwlIpcBackend);
      return true;
    }
    if (availableOrConnected(m_swayBackend, m_swayConnector)) {
      setActiveBackend(m_swayBackend);
      return true;
    }
    if (tryTriad()) {
      setActiveBackend(m_triadBackend);
      return true;
    }
    return false;
  };

  switch (compositors::detect()) {
  case compositors::CompositorKind::Hyprland:
    if (availableOrConnected(m_hyprlandBackend, m_hyprlandConnector)) {
      setActiveBackend(m_hyprlandBackend);
      return;
    }
    break;
  case compositors::CompositorKind::Mango:
    if (availableOrConnected(m_mangoIpcBackend, m_mangoIpcConnector)) {
      setActiveBackend(m_mangoIpcBackend);
      return;
    }
    kLog.warn("Mango detected but MANGO_INSTANCE_SIGNATURE socket IPC is missing or unusable");
    if (tryFallback(false, false)) {
      return;
    }
    setActiveBackend(nullptr);
    return;
  case compositors::CompositorKind::Dwl:
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
    if (tryTriad()) {
      setActiveBackend(m_triadBackend);
      return;
    }
    break;
  case compositors::CompositorKind::Kde:
    if (availableOrConnected(m_kwinBackend)) {
      setActiveBackend(m_kwinBackend);
      return;
    }
    break;
  case compositors::CompositorKind::Labwc:
    if (availableOrConnected(m_extBackend)) {
      setActiveBackend(m_extBackend);
      return;
    }
    break;
  case compositors::CompositorKind::Niri:
  case compositors::CompositorKind::Unknown:
    break;
  }

  if (tryFallback(true, true)) {
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

wl_output* WaylandWorkspaces::mangoIpcSelectedOutput() const {
  if (m_mangoIpcBackend == nullptr || !m_mangoIpcBackend->isAvailable()) {
    return nullptr;
  }
  return static_cast<MangoWorkspaceBackend*>(m_mangoIpcBackend)->ipcSelectedOutput();
}

std::optional<std::pair<std::string, std::string>>
WaylandWorkspaces::mangoIpcFocusedClientOnOutput(wl_output* output) const {
  if (m_mangoIpcBackend == nullptr || !m_mangoIpcBackend->isAvailable()) {
    return std::nullopt;
  }
  return static_cast<const MangoWorkspaceBackend*>(m_mangoIpcBackend)->ipcFocusedClientForOutput(output);
}

wl_output* WaylandWorkspaces::dwlIpcSelectedOutput() const {
  if (m_dwlIpcBackend == nullptr || !m_dwlIpcBackend->isAvailable()) {
    return nullptr;
  }
  return static_cast<DwlWorkspaceBackend*>(m_dwlIpcBackend)->ipcSelectedOutput();
}

std::optional<std::pair<std::string, std::string>>
WaylandWorkspaces::dwlIpcFocusedClientOnOutput(wl_output* output) const {
  if (m_dwlIpcBackend == nullptr || !m_dwlIpcBackend->isAvailable()) {
    return std::nullopt;
  }
  return static_cast<const DwlWorkspaceBackend*>(m_dwlIpcBackend)->ipcFocusedClientForOutput(output);
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
