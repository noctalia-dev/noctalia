#include "compositors/compositor_platform.h"

#include "compositors/compositor_detect.h"
#include "compositors/compositor_runtime.h"
#include "compositors/ext_workspace/ext_workspace_output_backend.h"
#include "compositors/hyprland/hyprland_keyboard_backend.h"
#include "compositors/hyprland/hyprland_output_backend.h"
#include "compositors/hyprland/hyprland_runtime.h"
#include "compositors/hyprland/hyprland_toplevel_mapping.h"
#include "compositors/hyprland/hyprland_window_id.h"
#include "compositors/mango/mango_keyboard_backend.h"
#include "compositors/mango/mango_output_backend.h"
#include "compositors/niri/niri_keyboard_backend.h"
#include "compositors/niri/niri_output_backend.h"
#include "compositors/niri/niri_runtime.h"
#include "compositors/niri/niri_workspace_backend.h"
#include "compositors/sway/sway_keyboard_backend.h"
#include "compositors/sway/sway_output_backend.h"
#include "compositors/sway/sway_runtime.h"
#include "compositors/triad/triad_keyboard_backend.h"
#include "compositors/triad/triad_output_backend.h"
#include "compositors/triad/triad_runtime.h"
#include "compositors/triad/triad_workspace_backend.h"
#include "core/log.h"
#include "core/process.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_workspaces.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <functional>
#include <json.hpp>
#include <optional>
#include <string>
#include <sys/types.h>
#include <unordered_set>
#include <utility>

namespace compositors {

  class FocusedOutputBackend {
  public:
    virtual ~FocusedOutputBackend() = default;
    [[nodiscard]] virtual std::optional<std::string> focusedOutputName() const = 0;
  };

  class OutputPowerBackend {
  public:
    virtual ~OutputPowerBackend() = default;
    [[nodiscard]] virtual bool setOutputPower(WaylandConnection& wayland, bool on) const = 0;
    [[nodiscard]] virtual bool isPerOutputTargeted() const noexcept { return false; }
  };

} // namespace compositors

namespace {

  constexpr Logger kLog("compositor_platform");

  [[nodiscard]] const char* valueOrUnset(const char* value) {
    return value != nullptr && value[0] != '\0' ? value : "<unset>";
  }

  void logSessionExitContext(compositors::CompositorKind compositor) {
    kLog.info(
        "logout requested: compositor={} env_hint=\"{}\" xdg_session_id={} user={}", compositors::name(compositor),
        compositors::envHint(), valueOrUnset(std::getenv("XDG_SESSION_ID")), valueOrUnset(std::getenv("USER"))
    );
  }

  void logLabwcExitFailure(std::string_view command, const process::RunResult& result) {
    if (!result.err.empty()) {
      kLog.warn("logout: {} failed with code {}: {}", command, result.exitCode, result.err);
    } else if (!result.out.empty()) {
      kLog.warn("logout: {} failed with code {}: {}", command, result.exitCode, result.out);
    } else {
      kLog.warn("logout: {} failed with code {}", command, result.exitCode);
    }
  }

  [[nodiscard]] bool terminateLabwcPid() {
    const char* pidEnv = std::getenv("LABWC_PID");
    if (pidEnv == nullptr || pidEnv[0] == '\0') {
      kLog.warn("logout: LABWC_PID is not set");
      return false;
    }

    errno = 0;
    char* end = nullptr;
    const long pid = std::strtol(pidEnv, &end, 10);
    if (errno != 0 || end == pidEnv || (end != nullptr && *end != '\0') || pid <= 1) {
      kLog.warn("logout: LABWC_PID has invalid value \"{}\"", pidEnv);
      return false;
    }

    if (::kill(static_cast<pid_t>(pid), SIGTERM) != 0) {
      kLog.warn("logout: failed to terminate LABWC_PID={}", pidEnv);
      return false;
    }
    return true;
  }

  [[nodiscard]] bool requestLabwcSessionExit() {
    if (process::commandExists("labwc")) {
      const process::RunResult longResult = process::runSync({"labwc", "--exit"});
      if (longResult) {
        return true;
      }
      logLabwcExitFailure("labwc --exit", longResult);

      const process::RunResult shortResult = process::runSync({"labwc", "-e"});
      if (shortResult) {
        return true;
      }
      logLabwcExitFailure("labwc -e", shortResult);
    } else {
      kLog.warn("logout: labwc executable not found");
    }

    return terminateLabwcPid();
  }

  [[nodiscard]] bool requestLoginSessionExit() {
    if (const char* sessionId = std::getenv("XDG_SESSION_ID"); sessionId != nullptr && sessionId[0] != '\0') {
      if (process::launchFirstAvailable({{"loginctl", "terminate-session", sessionId}})) {
        return true;
      }
    }
    if (process::launchFirstAvailable({{"systemctl", "--user", "stop", "graphical-session.target"}})) {
      return true;
    }
    if (const char* user = std::getenv("USER"); user != nullptr && user[0] != '\0') {
      if (process::launchFirstAvailable({{"loginctl", "terminate-user", user}})) {
        return true;
      }
    }
    return false;
  }

  void appendHyprlandExtOnlyWindows(
      std::vector<ToplevelInfo>& windows, const std::vector<ToplevelInfo>& extWindows,
      const compositors::hyprland::HyprlandToplevelMapping& mapping,
      const std::unordered_set<std::string>* outputWindowIds
  ) {
    std::unordered_set<std::string> wlrRepresentedIds;
    wlrRepresentedIds.reserve(windows.size());
    for (const auto& window : windows) {
      if (window.handle == nullptr) {
        continue;
      }
      const auto windowId = mapping.windowIdForWlrHandle(window.handle);
      if (!windowId.has_value()) {
        continue;
      }
      const auto normalized = compositors::hyprland::normalizeWindowId(*windowId);
      if (!normalized.empty()) {
        wlrRepresentedIds.insert(normalized);
      }
    }

    for (const auto& extWindow : extWindows) {
      if (extWindow.extHandle == nullptr) {
        continue;
      }
      const auto windowId = mapping.windowIdForExtHandle(extWindow.extHandle);
      if (windowId.has_value()) {
        const auto normalized = compositors::hyprland::normalizeWindowId(*windowId);
        // Pre-shell windows are often ext-only: mapping may know a wlr handle Hyprland never
        // exports via zwlr_foreign_toplevel_management, so dedupe only against live wlr results.
        if (!normalized.empty() && wlrRepresentedIds.contains(normalized)) {
          continue;
        }
        // ext_foreign_toplevel_list has no per-output metadata; scope to this bar's monitor via IPC.
        if (outputWindowIds != nullptr && (normalized.empty() || !outputWindowIds->contains(normalized))) {
          continue;
        }
      } else if (outputWindowIds != nullptr) {
        continue;
      }
      windows.push_back(extWindow);
    }
  }

  template <typename BackendT> class FocusedOutputAdapter final : public compositors::FocusedOutputBackend {
  public:
    FocusedOutputAdapter() = default;

    template <typename... Args>
    explicit FocusedOutputAdapter(Args&&... args) : m_backend(std::forward<Args>(args)...) {}

    [[nodiscard]] std::optional<std::string> focusedOutputName() const override {
      return m_backend.focusedOutputName();
    }

  private:
    BackendT m_backend;
  };

  template <typename BackendT> class KeyboardLayoutBackendAdapter final : public KeyboardLayoutBackend {
  public:
    KeyboardLayoutBackendAdapter() = default;

    template <typename... Args>
    explicit KeyboardLayoutBackendAdapter(Args&&... args) : m_backend(std::forward<Args>(args)...) {}

    [[nodiscard]] bool isAvailable() const noexcept override { return m_backend.isAvailable(); }
    [[nodiscard]] bool cycleLayout() const override { return m_backend.cycleLayout(); }
    [[nodiscard]] std::optional<KeyboardLayoutState> layoutState() const override { return m_backend.layoutState(); }
    [[nodiscard]] std::optional<std::string> currentLayoutName() const override {
      return m_backend.currentLayoutName();
    }

    bool connectSocket() override {
      if constexpr (requires { m_backend.connectSocket(); }) {
        return m_backend.connectSocket();
      }
      return false;
    }
    void setChangeCallback(ChangeCallback callback) override {
      if constexpr (requires { m_backend.setChangeCallback(std::move(callback)); }) {
        m_backend.setChangeCallback(std::move(callback));
      }
    }
    [[nodiscard]] int pollFd() const noexcept override {
      if constexpr (requires { m_backend.pollFd(); }) {
        return m_backend.pollFd();
      }
      return -1;
    }
    void dispatchPoll(short revents) override {
      if constexpr (requires { m_backend.dispatchPoll(revents); }) {
        m_backend.dispatchPoll(revents);
      }
    }
    void cleanup() override {
      if constexpr (requires { m_backend.cleanup(); }) {
        m_backend.cleanup();
      }
    }

  private:
    BackendT m_backend;
  };

  class LambdaOutputPowerBackend final : public compositors::OutputPowerBackend {
  public:
    using Callback = std::function<bool(WaylandConnection&, bool)>;

    explicit LambdaOutputPowerBackend(Callback callback, bool perOutputTargeted = false)
        : m_callback(std::move(callback)), m_perOutputTargeted(perOutputTargeted) {}

    [[nodiscard]] bool setOutputPower(WaylandConnection& wayland, bool on) const override {
      return m_callback && m_callback(wayland, on);
    }
    [[nodiscard]] bool isPerOutputTargeted() const noexcept override { return m_perOutputTargeted; }

  private:
    Callback m_callback;
    bool m_perOutputTargeted = false;
  };

  [[nodiscard]] bool setGenericOutputPower(WaylandConnection& /*wayland*/, bool on) {
    return compositors::ext_workspace::setOutputPower(on);
  }

  [[nodiscard]] std::unique_ptr<compositors::OutputPowerBackend>
  createOutputPowerBackend(compositors::CompositorRuntimeRegistry& runtimeRegistry) {
    switch (compositors::detect()) {
    case compositors::CompositorKind::Hyprland:
      return std::make_unique<LambdaOutputPowerBackend>(
          [&runtime = runtimeRegistry.hyprland()](WaylandConnection& /*wayland*/, bool on) {
            return compositors::hyprland::setOutputPower(runtime, on);
          }
      );
    case compositors::CompositorKind::Niri:
      return std::make_unique<LambdaOutputPowerBackend>([&runtime = runtimeRegistry.niri()](
                                                            WaylandConnection& /*wayland*/, bool on
                                                        ) { return compositors::niri::setOutputPower(runtime, on); });
    case compositors::CompositorKind::Sway:
      return std::make_unique<LambdaOutputPowerBackend>(
          [&runtime = runtimeRegistry.sway()](WaylandConnection& wayland, bool on) {
            (void)wayland;
            return compositors::sway::setOutputPower(runtime, on);
          }
      );
    case compositors::CompositorKind::Triad:
      return std::make_unique<LambdaOutputPowerBackend>([&runtime = runtimeRegistry.triad()](
                                                            WaylandConnection& /*wayland*/, bool on
                                                        ) { return compositors::triad::setOutputPower(runtime, on); });
    case compositors::CompositorKind::Mango:
      return std::make_unique<LambdaOutputPowerBackend>(
          [&runtime = runtimeRegistry.mango()](WaylandConnection& wayland, bool on) {
            return compositors::mango::setOutputPower(runtime, wayland, on);
          },
          true
      );
    case compositors::CompositorKind::Dwl:
    case compositors::CompositorKind::Labwc:
    case compositors::CompositorKind::Unknown:
      return std::make_unique<LambdaOutputPowerBackend>(&setGenericOutputPower);
    }
    return nullptr;
  }

  [[nodiscard]] std::unique_ptr<compositors::FocusedOutputBackend>
  createFocusedOutputBackend(compositors::CompositorRuntimeRegistry& runtimeRegistry) {
    switch (compositors::detect()) {
    case compositors::CompositorKind::Hyprland:
      return std::make_unique<FocusedOutputAdapter<HyprlandOutputBackend>>(runtimeRegistry.hyprland());
    case compositors::CompositorKind::Niri:
      return std::make_unique<FocusedOutputAdapter<NiriOutputBackend>>(runtimeRegistry.niri());
    case compositors::CompositorKind::Sway:
      return std::make_unique<FocusedOutputAdapter<SwayOutputBackend>>(runtimeRegistry.sway());
    case compositors::CompositorKind::Triad:
      return std::make_unique<FocusedOutputAdapter<TriadOutputBackend>>(runtimeRegistry.triad());
    case compositors::CompositorKind::Dwl:
    case compositors::CompositorKind::Labwc:
    case compositors::CompositorKind::Mango:
    case compositors::CompositorKind::Unknown:
      break;
    }
    return nullptr;
  }

  [[nodiscard]] std::unique_ptr<compositors::WorkspaceMetadataBackend>
  createWorkspaceMetadataBackend(compositors::CompositorRuntimeRegistry& runtimeRegistry) {
    switch (compositors::detect()) {
    case compositors::CompositorKind::Triad:
      return std::make_unique<TriadWorkspaceBackend>(runtimeRegistry.triad());
    case compositors::CompositorKind::Niri:
      return std::make_unique<NiriWorkspaceBackend>(runtimeRegistry.niri());
    case compositors::CompositorKind::Hyprland:
    case compositors::CompositorKind::Sway:
    case compositors::CompositorKind::Mango:
    case compositors::CompositorKind::Dwl:
    case compositors::CompositorKind::Labwc:
    case compositors::CompositorKind::Unknown:
      break;
    }
    return nullptr;
  }

  [[nodiscard]] std::unique_ptr<KeyboardLayoutBackend>
  createKeyboardLayoutBackend(compositors::CompositorRuntimeRegistry& runtimeRegistry) {
    switch (compositors::detect()) {
    case compositors::CompositorKind::Niri:
      return std::make_unique<KeyboardLayoutBackendAdapter<NiriKeyboardBackend>>(runtimeRegistry.niri());
    case compositors::CompositorKind::Hyprland:
      return std::make_unique<KeyboardLayoutBackendAdapter<HyprlandKeyboardBackend>>(runtimeRegistry.hyprland());
    case compositors::CompositorKind::Mango:
      return std::make_unique<KeyboardLayoutBackendAdapter<MangoKeyboardBackend>>(runtimeRegistry.mango());
    case compositors::CompositorKind::Sway:
      return std::make_unique<KeyboardLayoutBackendAdapter<SwayKeyboardBackend>>(runtimeRegistry.sway());
    case compositors::CompositorKind::Triad:
      return std::make_unique<KeyboardLayoutBackendAdapter<TriadKeyboardBackend>>(runtimeRegistry.triad());
    case compositors::CompositorKind::Dwl:
    case compositors::CompositorKind::Labwc:
    case compositors::CompositorKind::Unknown:
      break;
    }
    return nullptr;
  }

} // namespace

CompositorPlatform::CompositorPlatform(WaylandConnection& wayland)
    : m_wayland(wayland), m_runtimeRegistry(std::make_unique<compositors::CompositorRuntimeRegistry>()),
      m_workspaces(std::make_unique<WaylandWorkspaces>(*m_runtimeRegistry)) {
  m_workspaceMetadataBackend = createWorkspaceMetadataBackend(*m_runtimeRegistry);
  if (auto focusedOutputBackend = createFocusedOutputBackend(*m_runtimeRegistry); focusedOutputBackend != nullptr) {
    m_focusedOutputBackends.push_back(std::move(focusedOutputBackend));
  }
  m_outputPowerBackend = createOutputPowerBackend(*m_runtimeRegistry);
  m_keyboardLayoutBackend = createKeyboardLayoutBackend(*m_runtimeRegistry);

  m_workspaces->setOutputNameResolver([this](wl_output* output) { return connectorNameForOutput(output); });

  m_wayland.setWorkspaceManagerCallbacks(
      [this](ext_workspace_manager_v1* manager) { bindExtWorkspace(manager); },
      [this](zdwl_ipc_manager_v2* manager) { bindDwlIpcWorkspace(manager); }
  );
  m_wayland.setHyprlandToplevelMappingManagerCallback([this](hyprland_toplevel_mapping_manager_v1* manager) {
    bindHyprlandToplevelMappingManager(manager);
  });
  m_wayland.setToplevelChangeCallback([this]() { notifyToplevelsChanged(); });
  m_wayland.setOutputLifecycleCallbacks(
      [this](wl_output* output) { onOutputAdded(output); }, [this](wl_output* output) { onOutputRemoved(output); }
  );
}

CompositorPlatform::~CompositorPlatform() {
  cleanup();
  m_wayland.setOutputLifecycleCallbacks({}, {});
  m_wayland.setWorkspaceManagerCallbacks({}, {});
  m_wayland.setHyprlandToplevelMappingManagerCallback({});
  m_wayland.setToplevelChangeCallback({});
}

void CompositorPlatform::initialize() {
  if (m_initialized) {
    return;
  }
  m_initialized = true;

  m_workspaces->initialize();
  for (const auto& output : m_wayland.outputs()) {
    if (output.output != nullptr) {
      m_workspaces->onOutputAdded(output.output);
    }
  }
}

void CompositorPlatform::cleanup() {
  if (m_hyprlandToplevelMapping != nullptr) {
    m_hyprlandToplevelMapping->cleanup();
    m_hyprlandToplevelMapping.reset();
  }
  if (m_workspaceMetadataBackend != nullptr) {
    m_workspaceMetadataBackend->cleanup();
  }
  if (m_workspaces != nullptr) {
    m_workspaces->cleanup();
  }
  m_initialized = false;
}

wl_display* CompositorPlatform::display() const noexcept { return m_wayland.display(); }

compositors::niri::NiriRuntime& CompositorPlatform::niriRuntime() noexcept { return m_runtimeRegistry->niri(); }

const compositors::niri::NiriRuntime& CompositorPlatform::niriRuntime() const noexcept {
  return m_runtimeRegistry->niri();
}

bool CompositorPlatform::hasXdgShell() const noexcept { return m_wayland.hasXdgShell(); }

bool CompositorPlatform::hasXdgActivation() const noexcept { return m_wayland.hasXdgActivation(); }

std::string CompositorPlatform::requestActivationToken(wl_surface* surface) const {
  return m_wayland.requestActivationToken(surface);
}

bool CompositorPlatform::hasGammaControl() const noexcept { return m_wayland.hasGammaControl(); }

const std::vector<WaylandOutput>& CompositorPlatform::outputs() const noexcept { return m_wayland.outputs(); }

const WaylandOutput* CompositorPlatform::findOutputByWl(wl_output* output) const {
  return m_wayland.findOutputByWl(output);
}

wl_output* CompositorPlatform::outputForSurface(wl_surface* surface) const noexcept {
  return m_wayland.outputForSurface(surface);
}

FocusGrabService* CompositorPlatform::focusGrabService() const noexcept { return m_wayland.focusGrabService(); }

wl_surface* CompositorPlatform::lastPointerSurface() const noexcept { return m_wayland.lastPointerSurface(); }

wl_surface* CompositorPlatform::lastKeyboardSurface() const noexcept { return m_wayland.lastKeyboardSurface(); }

double CompositorPlatform::lastPointerX() const noexcept { return m_wayland.lastPointerX(); }

double CompositorPlatform::lastPointerY() const noexcept { return m_wayland.lastPointerY(); }

std::uint32_t CompositorPlatform::lastInputSerial() const noexcept { return m_wayland.lastInputSerial(); }

zwlr_layer_surface_v1* CompositorPlatform::layerSurfaceFor(wl_surface* surface) const noexcept {
  return m_wayland.layerSurfaceFor(surface);
}

void CompositorPlatform::stopKeyRepeat() { m_wayland.stopKeyRepeat(); }

void CompositorPlatform::setCursorShape(std::uint32_t serial, std::uint32_t shape) {
  m_wayland.setCursorShape(serial, shape);
}

wl_output* CompositorPlatform::preferredInteractiveOutput(std::chrono::milliseconds pointerMaxAge) const {
  if (compositors::detect() == compositors::CompositorKind::Mango && m_workspaces != nullptr) {
    if (wl_output* ipc = m_workspaces->mangoIpcSelectedOutput(); ipc != nullptr) {
      return ipc;
    }
  }
  if (compositors::detect() == compositors::CompositorKind::Dwl && m_workspaces != nullptr) {
    if (wl_output* ipc = m_workspaces->dwlIpcSelectedOutput(); ipc != nullptr) {
      return ipc;
    }
  }

  for (const auto& backend : m_focusedOutputBackends) {
    if (backend == nullptr) {
      continue;
    }
    if (const auto focusedName = backend->focusedOutputName(); focusedName.has_value()) {
      if (wl_output* output = resolveOutputName(*focusedName); output != nullptr) {
        return output;
      }
    }
  }

  if (wl_output* output = m_wayland.activeToplevelOutput(); output != nullptr) {
    return output;
  }

  if (wl_surface* keyboardSurface = m_wayland.lastKeyboardSurface(); keyboardSurface != nullptr) {
    if (wl_output* output = m_wayland.outputForSurface(keyboardSurface); output != nullptr) {
      return output;
    }
  }

  if (m_wayland.hasFreshPointerOutput(pointerMaxAge)) {
    return m_wayland.lastPointerOutput();
  }

  const auto& outputs = m_wayland.outputs();
  return !outputs.empty() ? outputs.front().output : nullptr;
}

std::optional<ActiveToplevel> CompositorPlatform::activeToplevel() const {
  if (compositors::detect() == compositors::CompositorKind::Mango && m_workspaces != nullptr) {
    wl_output* const selected = m_workspaces->mangoIpcSelectedOutput();
    if (selected != nullptr) {
      const auto hints = m_workspaces->mangoIpcFocusedClientOnOutput(selected);
      if (hints.has_value()) {
        const auto& [title, appId] = *hints;
        if (title.empty() && appId.empty()) {
          return std::nullopt;
        }
        if (auto matched = m_wayland.matchToplevelByTitleAndAppId(title, appId, selected); matched.has_value()) {
          return matched;
        }
        return ActiveToplevel{
            .title = title,
            .appId = appId,
            .identifier = appId + ":" + title,
            .handle = nullptr,
        };
      }
    }
  }
  if (compositors::detect() == compositors::CompositorKind::Dwl && m_workspaces != nullptr) {
    wl_output* const selected = m_workspaces->dwlIpcSelectedOutput();
    if (selected != nullptr) {
      const auto hints = m_workspaces->dwlIpcFocusedClientOnOutput(selected);
      if (hints.has_value()) {
        const auto& [title, appId] = *hints;
        if (title.empty() && appId.empty()) {
          return std::nullopt;
        }
        if (auto matched = m_wayland.matchToplevelByTitleAndAppId(title, appId, selected); matched.has_value()) {
          return matched;
        }
        return ActiveToplevel{
            .title = title,
            .appId = appId,
            .identifier = appId + ":" + title,
            .handle = nullptr,
        };
      }
    }
  }
  return m_wayland.activeToplevel();
}

wl_output* CompositorPlatform::activeToplevelOutput() const { return m_wayland.activeToplevelOutput(); }

std::vector<std::string> CompositorPlatform::runningAppIds(wl_output* outputFilter) const {
  return m_wayland.runningAppIds(outputFilter);
}

std::vector<ToplevelInfo> CompositorPlatform::windowsForApp(
    const std::string& idLower, const std::string& wmClassLower, wl_output* outputFilter
) const {
  auto windows = m_wayland.windowsForApp(idLower, wmClassLower, outputFilter);
  if (!compositors::isHyprland()
      || m_hyprlandToplevelMapping == nullptr
      || !m_hyprlandToplevelMapping->available()
      || !m_wayland.hasExtForeignToplevelList()) {
    return windows;
  }

  std::unordered_set<std::string> outputWindowIds;
  if (outputFilter != nullptr && m_workspaces != nullptr) {
    for (const auto& row : m_workspaces->workspaceWindows(outputFilter)) {
      const auto normalized = compositors::hyprland::normalizeWindowId(row.windowId);
      if (!normalized.empty()) {
        outputWindowIds.insert(normalized);
      }
    }
  }

  appendHyprlandExtOnlyWindows(
      windows, m_wayland.extWindowsForApp(idLower, wmClassLower), *m_hyprlandToplevelMapping,
      outputFilter != nullptr ? &outputWindowIds : nullptr
  );
  return windows;
}

void CompositorPlatform::activateToplevel(zwlr_foreign_toplevel_handle_v1* handle) {
  m_wayland.activateToplevel(handle);
}

void CompositorPlatform::closeToplevel(zwlr_foreign_toplevel_handle_v1* handle) { m_wayland.closeToplevel(handle); }

bool CompositorPlatform::containsWlrToplevelHandle(zwlr_foreign_toplevel_handle_v1* handle) const {
  return m_wayland.containsWlrToplevelHandle(handle);
}

void CompositorPlatform::setToplevelChangeCallback(ChangeCallback callback) {
  m_toplevelChangeCallback = std::move(callback);
}

void CompositorPlatform::bindHyprlandToplevelMappingManager(hyprland_toplevel_mapping_manager_v1* manager) {
  if (manager == nullptr) {
    if (m_hyprlandToplevelMapping != nullptr) {
      m_hyprlandToplevelMapping->cleanup();
      m_hyprlandToplevelMapping.reset();
    }
    return;
  }
  if (m_hyprlandToplevelMapping == nullptr) {
    m_hyprlandToplevelMapping = std::make_unique<compositors::hyprland::HyprlandToplevelMapping>();
    m_hyprlandToplevelMapping->setChangeCallback([this]() { notifyToplevelsChanged(); });
  }
  m_hyprlandToplevelMapping->initialize(manager);
  syncHyprlandToplevelMappings();
}

void CompositorPlatform::syncHyprlandToplevelMappings() {
  if (m_hyprlandToplevelMapping == nullptr || !m_hyprlandToplevelMapping->available()) {
    return;
  }
  std::vector<zwlr_foreign_toplevel_handle_v1*> wlrHandles;
  m_wayland.visitWlrToplevelHandles([&](zwlr_foreign_toplevel_handle_v1* handle) { wlrHandles.push_back(handle); });
  m_hyprlandToplevelMapping->syncWlrHandles(wlrHandles);

  if (compositors::isHyprland() && m_wayland.hasExtForeignToplevelList()) {
    std::vector<ext_foreign_toplevel_handle_v1*> extHandles;
    m_wayland.visitExtToplevelHandles([&](ext_foreign_toplevel_handle_v1* handle) { extHandles.push_back(handle); });
    m_hyprlandToplevelMapping->syncExtHandles(extHandles);
  }
}

void CompositorPlatform::notifyToplevelsChanged() {
  syncHyprlandToplevelMappings();
  if (m_toplevelChangeCallback) {
    m_toplevelChangeCallback();
  }
}

std::optional<std::string>
CompositorPlatform::compositorWindowIdForToplevel(zwlr_foreign_toplevel_handle_v1* handle) const {
  if (m_hyprlandToplevelMapping == nullptr) {
    return std::nullopt;
  }
  return m_hyprlandToplevelMapping->windowIdForWlrHandle(handle);
}

std::optional<std::string>
CompositorPlatform::compositorWindowIdForExtToplevel(ext_foreign_toplevel_handle_v1* handle) const {
  if (m_hyprlandToplevelMapping == nullptr) {
    return std::nullopt;
  }
  return m_hyprlandToplevelMapping->windowIdForExtHandle(handle);
}

zwlr_foreign_toplevel_handle_v1*
CompositorPlatform::toplevelHandleForCompositorWindowId(const std::string_view windowId) const {
  if (m_hyprlandToplevelMapping == nullptr) {
    return nullptr;
  }
  return m_hyprlandToplevelMapping->wlrHandleForWindowId(windowId);
}

bool CompositorPlatform::isCompositorWindowIdKnown(const std::string_view windowId) const {
  if (m_hyprlandToplevelMapping == nullptr || !m_hyprlandToplevelMapping->available()) {
    return false;
  }

  const auto normalized = compositors::hyprland::normalizeWindowId(windowId);
  if (normalized.empty()) {
    return false;
  }

  if (const auto* wlrHandle = m_hyprlandToplevelMapping->wlrHandleForWindowId(normalized)) {
    bool live = false;
    m_wayland.visitWlrToplevelHandles([&](zwlr_foreign_toplevel_handle_v1* handle) {
      if (handle == wlrHandle) {
        live = true;
      }
    });
    if (live) {
      return true;
    }
  }

  if (const auto* extHandle = m_hyprlandToplevelMapping->extHandleForWindowId(normalized)) {
    bool live = false;
    m_wayland.visitExtToplevelHandles([&](ext_foreign_toplevel_handle_v1* handle) {
      if (handle == extHandle) {
        live = true;
      }
    });
    if (live) {
      return true;
    }
  }

  return false;
}

std::optional<std::string> CompositorPlatform::focusedCompositorWindowId() const {
  if (m_workspaces == nullptr) {
    return std::nullopt;
  }
  return m_workspaces->focusedWindowId();
}

void CompositorPlatform::setWorkspaceChangeCallback(ChangeCallback callback) {
  m_workspaceChangeCallback = std::move(callback);
  m_lastWorkspaceModelSnapshot = workspaceModelSnapshot();
  auto wrapper = [this]() {
    auto nextSnapshot = workspaceModelSnapshot();
    if (sameWorkspaceModelSnapshot(nextSnapshot, m_lastWorkspaceModelSnapshot)) {
      return;
    }
    m_lastWorkspaceModelSnapshot = std::move(nextSnapshot);
    if (m_workspaceChangeCallback) {
      m_workspaceChangeCallback();
    }
  };

  if (m_workspaces != nullptr) {
    m_workspaces->setChangeCallback(wrapper);
  }
  if (m_workspaceMetadataBackend != nullptr) {
    m_workspaceMetadataBackend->setChangeCallback(wrapper);
  }
}

void CompositorPlatform::setOverviewChangeCallback(ChangeCallback callback) {
  m_overviewChangeCallback = std::move(callback);
  if (m_workspaceMetadataBackend != nullptr) {
    m_workspaceMetadataBackend->setOverviewChangeCallback(m_overviewChangeCallback);
  }
}

void CompositorPlatform::activateWorkspace(const std::string& id) {
  if (m_workspaces != nullptr) {
    m_workspaces->activate(id);
  }
}

void CompositorPlatform::activateWorkspace(wl_output* output, const std::string& id) {
  if (m_workspaces != nullptr) {
    m_workspaces->activateForOutput(output, id);
  }
}

void CompositorPlatform::activateWorkspace(wl_output* output, const Workspace& workspace) {
  if (m_workspaces != nullptr) {
    m_workspaces->activateForOutput(output, workspace);
  }
}

std::size_t CompositorPlatform::addWorkspacePollFds(std::vector<pollfd>& fds) const {
  const auto start = fds.size();
  if (m_workspaces != nullptr && m_workspaces->pollFd() >= 0) {
    fds.push_back({.fd = m_workspaces->pollFd(), .events = m_workspaces->pollEvents(), .revents = 0});
  }
  if (m_workspaceMetadataBackend != nullptr && m_workspaceMetadataBackend->pollFd() >= 0) {
    fds.push_back(
        {.fd = m_workspaceMetadataBackend->pollFd(), .events = m_workspaceMetadataBackend->pollEvents(), .revents = 0}
    );
  }
  return start;
}

int CompositorPlatform::workspacePollTimeoutMs() const noexcept {
  int timeout = m_workspaces != nullptr ? m_workspaces->pollTimeoutMs() : -1;
  if (m_workspaceMetadataBackend != nullptr) {
    const int trackerTimeout = m_workspaceMetadataBackend->pollTimeoutMs();
    if (trackerTimeout >= 0 && (timeout < 0 || trackerTimeout < timeout)) {
      timeout = trackerTimeout;
    }
  }
  return timeout;
}

void CompositorPlatform::dispatchWorkspacePoll(const std::vector<pollfd>& fds, std::size_t startIdx) {
  std::size_t index = startIdx;
  if (m_workspaces != nullptr
      && m_workspaces->pollFd() >= 0
      && index < fds.size()
      && fds[index].fd == m_workspaces->pollFd()) {
    m_workspaces->dispatchPoll(fds[index].revents);
    ++index;
  }

  if (m_workspaceMetadataBackend != nullptr) {
    short revents = 0;
    if (m_workspaceMetadataBackend->pollFd() >= 0
        && index < fds.size()
        && fds[index].fd == m_workspaceMetadataBackend->pollFd()) {
      revents = fds[index].revents;
    }
    m_workspaceMetadataBackend->dispatchPoll(revents);
  }
}

std::vector<Workspace> CompositorPlatform::workspaces() const {
  auto current = m_workspaces != nullptr ? m_workspaces->all() : std::vector<Workspace>{};
  if (m_workspaceMetadataBackend != nullptr) {
    m_workspaceMetadataBackend->apply(current);
  }
  return current;
}

std::vector<Workspace> CompositorPlatform::workspaces(wl_output* output) const {
  auto current = m_workspaces != nullptr ? m_workspaces->forOutput(output) : std::vector<Workspace>{};
  if (m_workspaceMetadataBackend != nullptr) {
    m_workspaceMetadataBackend->apply(current, connectorNameForOutput(output));
  }
  return current;
}

std::unordered_map<std::string, std::vector<std::string>>
CompositorPlatform::appIdsByWorkspace(wl_output* outputFilter) const {
  if (m_workspaceMetadataBackend != nullptr) {
    const auto fromMetadata = m_workspaceMetadataBackend->appIdsByWorkspace(connectorNameForOutput(outputFilter));
    if (!fromMetadata.empty()) {
      return fromMetadata;
    }
  }
  return m_workspaces != nullptr ? m_workspaces->appIdsByWorkspace(outputFilter)
                                 : std::unordered_map<std::string, std::vector<std::string>>{};
}

std::vector<std::string> CompositorPlatform::workspaceDisplayKeys(wl_output* outputFilter) const {
  if (m_workspaceMetadataBackend == nullptr) {
    return {};
  }
  return m_workspaceMetadataBackend->workspaceKeys(connectorNameForOutput(outputFilter));
}

std::vector<WorkspaceWindowAssignment> CompositorPlatform::workspaceWindowAssignments(wl_output* outputFilter) const {
  std::vector<WorkspaceWindow> windows;
  if (m_workspaceMetadataBackend != nullptr) {
    windows = m_workspaceMetadataBackend->workspaceWindows(connectorNameForOutput(outputFilter));
  }
  if (windows.empty() && m_workspaces != nullptr) {
    windows = m_workspaces->workspaceWindows(outputFilter);
  }

  std::vector<WorkspaceWindowAssignment> result;
  result.reserve(windows.size());
  for (const auto& window : windows) {
    result.push_back(
        WorkspaceWindowAssignment{
            .windowId = window.windowId,
            .workspaceKey = window.workspaceKey,
            .appId = window.appId,
            .title = window.title,
            .x = window.x,
            .y = window.y,
        }
    );
  }
  return result;
}

TaskbarAssignmentMode CompositorPlatform::taskbarAssignmentMode() const noexcept {
  return m_workspaces != nullptr ? m_workspaces->taskbarAssignmentMode() : TaskbarAssignmentMode::Generic;
}

std::unordered_map<std::uintptr_t, WorkspaceWindow> CompositorPlatform::assignTaskbarWindows(
    const std::vector<TaskbarWindowCandidate>& windows, wl_output* outputFilter
) const {
  return m_workspaces != nullptr ? m_workspaces->assignTaskbarWindows(windows, outputFilter)
                                 : std::unordered_map<std::uintptr_t, WorkspaceWindow>{};
}

const char* CompositorPlatform::workspaceBackendName() const noexcept {
  return m_workspaces != nullptr ? m_workspaces->backendName() : "none";
}

void CompositorPlatform::focusCompositorWindow(const std::string& windowId) const {
  if (m_workspaceMetadataBackend != nullptr && m_workspaceMetadataBackend->focusWindowById(windowId)) {
    return;
  }
  if (m_workspaces != nullptr) {
    m_workspaces->focusWindow(windowId);
  }
}

bool CompositorPlatform::cycleKeyboardLayout() const {
  return m_keyboardLayoutBackend != nullptr && m_keyboardLayoutBackend->cycleLayout();
}

bool CompositorPlatform::hasKeyboardLayoutBackend() const noexcept {
  return m_keyboardLayoutBackend != nullptr && m_keyboardLayoutBackend->isAvailable();
}

std::optional<KeyboardLayoutState> CompositorPlatform::keyboardLayoutState() const {
  if (m_keyboardLayoutBackend == nullptr) {
    return std::nullopt;
  }
  return m_keyboardLayoutBackend->layoutState();
}

std::string CompositorPlatform::currentKeyboardLayoutName() const {
  if (m_keyboardLayoutBackend != nullptr) {
    if (auto backendName = m_keyboardLayoutBackend->currentLayoutName();
        backendName.has_value() && !backendName->empty()) {
      return *backendName;
    }
  }
  return m_wayland.currentKeyboardLayoutName();
}

std::vector<std::string> CompositorPlatform::keyboardLayoutNames() const {
  if (m_keyboardLayoutBackend != nullptr) {
    if (const auto state = m_keyboardLayoutBackend->layoutState(); state.has_value() && !state->names.empty()) {
      if (state->names.size() > 1) {
        return state->names;
      }
      auto waylandNames = m_wayland.keyboardLayoutNames();
      return waylandNames.size() > state->names.size() ? waylandNames : state->names;
    }
  }
  return m_wayland.keyboardLayoutNames();
}

void CompositorPlatform::setKeyboardLayoutChangeCallback(ChangeCallback callback) {
  m_keyboardLayoutChangeCallback = std::move(callback);
  if (m_keyboardLayoutBackend != nullptr) {
    m_keyboardLayoutBackend->setChangeCallback(m_keyboardLayoutChangeCallback);
    m_keyboardLayoutBackend->connectSocket();
  }
}

void CompositorPlatform::addKeyboardLayoutPollFds(std::vector<pollfd>& fds) const {
  if (m_keyboardLayoutBackend != nullptr && m_keyboardLayoutBackend->pollFd() >= 0) {
    fds.push_back(
        {.fd = m_keyboardLayoutBackend->pollFd(), .events = m_keyboardLayoutBackend->pollEvents(), .revents = 0}
    );
  }
}

void CompositorPlatform::dispatchKeyboardLayoutPoll(const std::vector<pollfd>& fds, std::size_t startIdx) {
  if (m_keyboardLayoutBackend != nullptr && m_keyboardLayoutBackend->pollFd() >= 0 && startIdx < fds.size()) {
    m_keyboardLayoutBackend->dispatchPoll(fds[startIdx].revents);
  }
}

bool CompositorPlatform::requestSessionExit() const {
  const compositors::CompositorKind compositor = compositors::detect();
  logSessionExitContext(compositor);

  switch (compositor) {
  case compositors::CompositorKind::Hyprland: {
    auto& runtime = m_runtimeRegistry->hyprland();
    if (runtime.configIsLua()) {
      return runtime.request("dispatch hl.dsp.exit()") != std::nullopt;
    }
    return runtime.request("dispatch exit") != std::nullopt;
  }
  case compositors::CompositorKind::Sway: {
    const auto& command = m_runtimeRegistry->sway().msgCommand();
    return !command.empty() && process::runAsync(std::vector<std::string>{command, "exit"});
  }
  case compositors::CompositorKind::Niri:
    return m_runtimeRegistry->niri().requestAction(
        nlohmann::json{{"Quit", nlohmann::json{{"skip_confirmation", true}}}}, true
    );
  case compositors::CompositorKind::Triad:
    return m_runtimeRegistry->triad().requestAction("exit-session");
  case compositors::CompositorKind::Mango:
    return process::launchFirstAvailable({{"mmsg", "dispatch", "quit"}});
  case compositors::CompositorKind::Dwl:
    break;
  case compositors::CompositorKind::Labwc:
    if (requestLabwcSessionExit()) {
      return true;
    }
    break;
  case compositors::CompositorKind::Unknown:
    break;
  }

  return requestLoginSessionExit();
}

bool CompositorPlatform::setOutputPower(bool on) const {
  if (m_outputPowerBackend == nullptr) {
    return false;
  }
  if (m_outputPowerBackend->isPerOutputTargeted()) {
    m_lastRequestedOutputPowerState = on;
  }
  return m_outputPowerBackend->setOutputPower(m_wayland, on);
}

bool CompositorPlatform::tracksOverviewState() const noexcept {
  return m_workspaceMetadataBackend != nullptr && m_workspaceMetadataBackend->canTrackOverviewState();
}

bool CompositorPlatform::hasOverviewState() const noexcept {
  return m_workspaceMetadataBackend != nullptr && m_workspaceMetadataBackend->hasOverviewState();
}

bool CompositorPlatform::isOverviewOpen() const noexcept {
  if (m_workspaceMetadataBackend == nullptr || !m_workspaceMetadataBackend->hasOverviewState()) {
    return true;
  }
  return m_workspaceMetadataBackend->isOverviewOpen();
}

void CompositorPlatform::bindExtWorkspace(ext_workspace_manager_v1* manager) {
  if (m_workspaces != nullptr) {
    m_workspaces->bindExtWorkspace(manager);
  }
}

void CompositorPlatform::bindDwlIpcWorkspace(zdwl_ipc_manager_v2* manager) {
  if (m_workspaces != nullptr) {
    m_workspaces->bindDwlIpcWorkspace(manager);
  }
}

void CompositorPlatform::onOutputAdded(wl_output* output) {
  if (m_workspaces != nullptr) {
    m_workspaces->onOutputAdded(output);
  }
  if (m_outputPowerBackend != nullptr
      && m_outputPowerBackend->isPerOutputTargeted()
      && m_lastRequestedOutputPowerState.has_value()) {
    (void)m_outputPowerBackend->setOutputPower(m_wayland, *m_lastRequestedOutputPowerState);
  }
}

void CompositorPlatform::onOutputRemoved(wl_output* output) {
  if (m_workspaces != nullptr) {
    m_workspaces->onOutputRemoved(output);
  }
}

wl_output* CompositorPlatform::resolveOutputName(const std::string& outputName) const {
  if (outputName.empty()) {
    return nullptr;
  }
  for (const auto& output : m_wayland.outputs()) {
    if (output.output == nullptr) {
      continue;
    }
    if (output.connectorName == outputName || output.description == outputName) {
      return output.output;
    }
  }
  return nullptr;
}

std::string CompositorPlatform::connectorNameForOutput(wl_output* output) const {
  if (output == nullptr) {
    return {};
  }
  for (const auto& candidate : m_wayland.outputs()) {
    if (candidate.output == output) {
      return candidate.connectorName;
    }
  }
  return {};
}

std::vector<CompositorPlatform::WorkspaceModelSnapshot> CompositorPlatform::workspaceModelSnapshot() const {
  auto sortedAssignments = [](std::vector<WorkspaceWindowAssignment> assignments) {
    std::sort(assignments.begin(), assignments.end(), [](const auto& lhs, const auto& rhs) {
      if (lhs.windowId != rhs.windowId) {
        return lhs.windowId < rhs.windowId;
      }
      if (lhs.workspaceKey != rhs.workspaceKey) {
        return lhs.workspaceKey < rhs.workspaceKey;
      }
      return lhs.appId < rhs.appId;
    });
    return assignments;
  };

  auto makeSnapshot = [&](const WaylandOutput* output) {
    auto* wlOutput = output != nullptr ? output->output : nullptr;
    return WorkspaceModelSnapshot{
        .outputName = output != nullptr ? output->name : 0,
        .workspaces = workspaces(wlOutput),
        .assignments = sortedAssignments(workspaceWindowAssignments(wlOutput)),
    };
  };

  std::vector<WorkspaceModelSnapshot> snapshot;
  const auto& outputs = m_wayland.outputs();
  if (outputs.empty()) {
    snapshot.push_back(makeSnapshot(nullptr));
    return snapshot;
  }

  snapshot.reserve(outputs.size());
  for (const auto& output : outputs) {
    snapshot.push_back(makeSnapshot(&output));
  }
  return snapshot;
}

bool CompositorPlatform::sameWorkspaceModelSnapshot(
    const std::vector<WorkspaceModelSnapshot>& lhs, const std::vector<WorkspaceModelSnapshot>& rhs
) {
  auto sameWorkspace = [](const Workspace& a, const Workspace& b) {
    return a.id == b.id
        && a.name == b.name
        && a.coordinates == b.coordinates
        && a.active == b.active
        && a.urgent == b.urgent
        && a.occupied == b.occupied;
  };
  auto sameAssignment = [](const WorkspaceWindowAssignment& a, const WorkspaceWindowAssignment& b) {
    return a.windowId == b.windowId
        && a.workspaceKey == b.workspaceKey
        && a.appId == b.appId
        && a.title == b.title
        && a.x == b.x
        && a.y == b.y;
  };

  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    if (lhs[i].outputName != rhs[i].outputName
        || lhs[i].workspaces.size() != rhs[i].workspaces.size()
        || lhs[i].assignments.size() != rhs[i].assignments.size()) {
      return false;
    }
    for (std::size_t w = 0; w < lhs[i].workspaces.size(); ++w) {
      if (!sameWorkspace(lhs[i].workspaces[w], rhs[i].workspaces[w])) {
        return false;
      }
    }
    for (std::size_t a = 0; a < lhs[i].assignments.size(); ++a) {
      if (!sameAssignment(lhs[i].assignments[a], rhs[i].assignments[a])) {
        return false;
      }
    }
  }
  return true;
}
