#include "wayland/wayland_connection.h"

#include "compositors/compositor_detect.h"
#include "core/log.h"
#include "core/process/process_fds.h"
#include "cursor-shape-v1-client-protocol.h"
#include "dwl-ipc-unstable-v2-client-protocol.h"
#include "ext-background-effect-v1-client-protocol.h"
#include "ext-data-control-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-idle-notify-v1-client-protocol.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "ext-workspace-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "hyprland-focus-grab-v1-client-protocol.h"
#include "hyprland-toplevel-mapping-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "org-kde-plasma-virtual-desktop-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "util/string_utils.h"
#include "viewporter-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wayland/clipboard_service.h"
#include "wayland/hyprland/focus_grab_service.h"
#include "wayland/text_input_service.h"
#include "wayland/virtual_keyboard_service.h"
#include "wlr-data-control-unstable-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "wlr-gamma-control-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <format>
#include <stdexcept>
#include <utility>

namespace {

  constexpr std::uint32_t kCompositorVersion = 4;
  constexpr std::uint32_t kSeatVersion = 5;
  constexpr std::uint32_t kShmVersion = 1;
  constexpr std::uint32_t kSubcompositorVersion = 1;
  constexpr std::uint32_t kLayerShellVersion = 4;
  constexpr std::uint32_t kXdgOutputManagerVersion = 3;
  constexpr std::uint32_t kXdgWmBaseVersion = 6;
  constexpr std::uint32_t kExtWorkspaceManagerVersion = 1;
  constexpr std::uint32_t kKdeVirtualDesktopManagementVersion = 4;
  constexpr std::uint32_t kWlrForeignToplevelManagerVersion = 3;
  constexpr std::uint32_t kExtForeignToplevelListVersion = 1;
  constexpr std::uint32_t kCursorShapeManagerVersion = 1;
  constexpr std::uint32_t kXdgActivationVersion = 1;
  constexpr std::uint32_t kExtSessionLockManagerVersion = 1;
  constexpr std::uint32_t kExtIdleNotifierVersion = 2;
  constexpr std::uint32_t kIdleInhibitManagerVersion = 1;
  constexpr std::uint32_t kExtBackgroundEffectManagerVersion = 1;
  constexpr std::uint32_t kFractionalScaleManagerVersion = 1;
  constexpr std::uint32_t kHyprlandFocusGrabManagerVersion = 1;
  constexpr std::uint32_t kHyprlandToplevelMappingManagerVersion = 1;
  constexpr std::uint32_t kViewporterVersion = 1;
  constexpr std::uint32_t kOutputVersion = 4;
  constexpr std::uint32_t kTextInputManagerVersion = 2;
  constexpr std::uint32_t kVirtualKeyboardManagerVersion = 1;
  constexpr std::uint32_t kGammaControlManagerVersion = 1;
  constexpr std::uint32_t kScreencopyManagerVersion = 3;

  const wl_registry_listener kRegistryListener = {
      .global = &WaylandConnection::handleGlobal,
      .global_remove = &WaylandConnection::handleGlobalRemove,
  };

  std::string errnoText(int value) {
    if (value == 0) {
      return "none";
    }
    const char* text = std::strerror(value);
    return text != nullptr ? std::string(text) : std::string("unknown");
  }

  struct DetectedOutputScale {
    double scale = 0.0;
    bool rotated = false;
    bool available = false;
  };

  DetectedOutputScale detectOutputScale(const WaylandOutput& output) {
    if (output.width <= 0 || output.height <= 0 || output.logicalWidth <= 0 || output.logicalHeight <= 0) {
      return {};
    }

    // wl_output.mode is physical buffer pixels; xdg_output.logical_size is the
    // compositor's logical coordinate space. Their ratio is the output scale.
    struct Candidate {
      double scale = 0.0;
      double axisDelta = 0.0;
      bool rotated = false;
    };

    const auto candidate = [](double xScale, double yScale, bool rotated) {
      return Candidate{
          .scale = (xScale + yScale) * 0.5,
          .axisDelta = std::abs(xScale - yScale),
          .rotated = rotated,
      };
    };

    const auto physicalW = static_cast<double>(output.width);
    const auto physicalH = static_cast<double>(output.height);
    const auto logicalW = static_cast<double>(output.logicalWidth);
    const auto logicalH = static_cast<double>(output.logicalHeight);

    const Candidate normal = candidate(physicalW / logicalW, physicalH / logicalH, false);
    const Candidate rotated = candidate(physicalW / logicalH, physicalH / logicalW, true);
    const Candidate& selected = rotated.axisDelta < normal.axisDelta ? rotated : normal;
    if (selected.scale <= 0.0) {
      return {};
    }
    return {.scale = selected.scale, .rotated = selected.rotated, .available = true};
  }

  std::string outputLabel(const WaylandOutput& output) {
    if (!output.connectorName.empty()) {
      return output.connectorName;
    }
    return std::format("#{}", output.name);
  }

  void
  backgroundEffectCapabilities(void* data, ext_background_effect_manager_v1* /*manager*/, std::uint32_t capabilities) {
    auto* self = static_cast<WaylandConnection*>(data);
    self->onBackgroundEffectCapabilities(capabilities);
  }

  const ext_background_effect_manager_v1_listener kBackgroundEffectListener = {
      .capabilities = &backgroundEffectCapabilities,
  };

  void outputGeometry(
      void* data, wl_output* wlOut, int32_t /*x*/, int32_t /*y*/, int32_t /*physW*/, int32_t /*physH*/,
      int32_t /*subpixel*/, const char* /*make*/, const char* /*model*/, int32_t transform
  ) {
    auto* out = static_cast<WaylandConnection*>(data)->findOutputByWl(wlOut);
    if (out != nullptr) {
      out->transform = transform;
    }
  }

  void outputMode(void* data, wl_output* wlOut, uint32_t flags, int32_t w, int32_t h, int32_t /*refresh*/) {
    if ((flags & WL_OUTPUT_MODE_CURRENT) == 0) {
      return;
    }
    auto* out = static_cast<WaylandConnection*>(data)->findOutputByWl(wlOut);
    if (out != nullptr) {
      out->width = w;
      out->height = h;
    }
  }

  void outputDone(void* data, wl_output* wlOut) {
    auto* self = static_cast<WaylandConnection*>(data);
    auto* out = self->findOutputByWl(wlOut);
    if (out != nullptr) {
      out->done = true;
      self->notifyOutputReady(wlOut);
    }
  }

  void outputScale(void* data, wl_output* wlOut, int32_t factor) {
    auto* out = static_cast<WaylandConnection*>(data)->findOutputByWl(wlOut);
    if (out != nullptr) {
      out->scale = factor;
    }
  }

  void outputName(void* data, wl_output* wlOut, const char* name) {
    auto* self = static_cast<WaylandConnection*>(data);
    auto* out = self->findOutputByWl(wlOut);
    if (out != nullptr && name != nullptr) {
      const std::string nextName = name;
      if (out->connectorName == nextName) {
        return;
      }
      out->connectorName = nextName;

      if (out->done) {
        self->notifyOutputReady(wlOut);
      }
    }
  }

  void outputDescription(void* data, wl_output* wlOut, const char* desc) {
    auto* out = static_cast<WaylandConnection*>(data)->findOutputByWl(wlOut);
    if (out != nullptr) {
      out->description = desc;
    }
  }

  const wl_output_listener kOutputListener = {
      .geometry = outputGeometry,
      .mode = outputMode,
      .done = outputDone,
      .scale = outputScale,
      .name = outputName,
      .description = outputDescription,
  };

  void xdgOutputLogicalPosition(void* data, zxdg_output_v1* xdgOutput, int32_t x, int32_t y) {
    auto* out = static_cast<WaylandConnection*>(data)->findOutputByXdg(xdgOutput);
    if (out != nullptr) {
      out->logicalX = x;
      out->logicalY = y;
    }
  }

  void xdgOutputLogicalSize(void* data, zxdg_output_v1* xdgOutput, int32_t w, int32_t h) {
    auto* out = static_cast<WaylandConnection*>(data)->findOutputByXdg(xdgOutput);
    if (out != nullptr) {
      out->logicalWidth = w;
      out->logicalHeight = h;
    }
  }

  void xdgOutputDone(void* data, zxdg_output_v1* xdgOutput) {
    auto* self = static_cast<WaylandConnection*>(data);
    auto* out = self->findOutputByXdg(xdgOutput);
    if (out != nullptr && out->output != nullptr) {
      self->notifyOutputReady(out->output);
    }
  }

  void xdgOutputName(void* /*data*/, zxdg_output_v1* /*xdgOutput*/, const char* /*name*/) {}

  void xdgOutputDescription(void* /*data*/, zxdg_output_v1* /*xdgOutput*/, const char* /*desc*/) {}

  const zxdg_output_v1_listener kXdgOutputListener = {
      .logical_position = xdgOutputLogicalPosition,
      .logical_size = xdgOutputLogicalSize,
      .done = xdgOutputDone,
      .name = xdgOutputName,
      .description = xdgOutputDescription,
  };

  constexpr Logger kLog("wayland");

  void xdgWmBasePing(void* /*data*/, xdg_wm_base* wmBase, std::uint32_t serial) { xdg_wm_base_pong(wmBase, serial); }

  const xdg_wm_base_listener kXdgWmBaseListener = {
      .ping = xdgWmBasePing,
  };

} // namespace

WaylandConnection::WaylandConnection() = default;

WaylandConnection::~WaylandConnection() { cleanup(); }

bool WaylandConnection::connect() {
  if (m_display != nullptr) {
    return true;
  }

  m_display = wl_display_connect(nullptr);
  if (m_display == nullptr) {
    throw std::runtime_error("failed to connect to Wayland display");
  }

  m_registry = wl_display_get_registry(m_display);
  if (m_registry == nullptr) {
    cleanup();
    throw std::runtime_error("failed to acquire Wayland registry");
  }

  if (wl_registry_add_listener(m_registry, &kRegistryListener, this) != 0) {
    cleanup();
    throw std::runtime_error("failed to add Wayland registry listener");
  }

  if (wl_display_roundtrip(m_display) < 0) {
    const int roundtripErrno = errno;
    const std::string detail = describeDisplayError(roundtripErrno);
    cleanup();
    throw std::runtime_error(std::format("failed during Wayland registry roundtrip: {}", detail));
  }

  if (wl_display_roundtrip(m_display) < 0) {
    const int roundtripErrno = errno;
    const std::string detail = describeDisplayError(roundtripErrno);
    cleanup();
    throw std::runtime_error(std::format("failed during Wayland output discovery roundtrip: {}", detail));
  }

  m_focusGrabService = std::make_unique<FocusGrabService>();
  m_focusGrabService->initialize(m_hyprlandFocusGrabManager);

  logStartupSummary();
  return true;
}

void WaylandConnection::setOutputChangeCallback(ChangeCallback callback) {
  m_outputChangeCallback = std::move(callback);
}

void WaylandConnection::setOutputLifecycleCallbacks(
    std::function<void(wl_output*)> added, std::function<void(wl_output*)> removed
) {
  m_outputAddedCallback = std::move(added);
  m_outputRemovedCallback = std::move(removed);
}

void WaylandConnection::setWorkspaceManagerCallbacks(
    std::function<void(ext_workspace_manager_v1*)> extWorkspace, std::function<void(zdwl_ipc_manager_v2*)> dwlIpc
) {
  m_extWorkspaceManagerCallback = std::move(extWorkspace);
  m_dwlIpcManagerCallback = std::move(dwlIpc);
}

void WaylandConnection::setKdeVirtualDesktopManagerCallback(
    std::function<void(org_kde_plasma_virtual_desktop_management*)> callback
) {
  m_kdeVirtualDesktopManagerCallback = std::move(callback);
}

void WaylandConnection::setToplevelChangeCallback(ChangeCallback callback) {
  m_toplevelsHandler.setChangeCallback(callback);
  m_extForeignToplevels.setChangeCallback(std::move(callback));
}

void WaylandConnection::setHyprlandToplevelMappingManagerCallback(
    std::function<void(hyprland_toplevel_mapping_manager_v1* manager)> callback
) {
  m_hyprlandToplevelMappingManagerCallback = std::move(callback);
}

void WaylandConnection::setIdleCapabilitiesReadyCallback(ChangeCallback callback) {
  m_idleCapabilitiesReadyCallback = std::move(callback);
  notifyIdleCapabilitiesReady();
}

void WaylandConnection::notifyIdleCapabilitiesReady() {
  if (m_idleNotifier == nullptr || m_seat == nullptr || !m_idleCapabilitiesReadyCallback) {
    return;
  }
  m_idleCapabilitiesReadyCallback();
}

void WaylandConnection::setPointerEventCallback(WaylandSeat::PointerEventCallback callback) {
  m_pointerEventCallback = std::move(callback);
  m_seatHandler.setPointerEventCallback([this](const PointerEvent& event) {
    const auto it = m_surfaceOutputMap.find(event.surface);
    if (it != m_surfaceOutputMap.end() && it->second != nullptr) {
      m_lastPointerOutput = it->second;
      m_lastPointerOutputAt = std::chrono::steady_clock::now();
    }
    if (m_pointerEventCallback) {
      m_pointerEventCallback(event);
    }
  });
}

void WaylandConnection::registerSurfaceOutput(wl_surface* surface, wl_output* output) {
  if (surface == nullptr || output == nullptr) {
    return;
  }
  m_surfaceOutputMap[surface] = output;
}

void WaylandConnection::notifySurfaceOutputEnter(wl_surface* surface, wl_output* output) {
  if (surface == nullptr || output == nullptr) {
    return;
  }
  auto& outputs = m_surfaceOutputs[surface];
  if (!std::ranges::contains(outputs, output)) {
    outputs.push_back(output);
  }
  m_surfaceOutputMap[surface] = output;
}

void WaylandConnection::notifySurfaceOutputLeave(wl_surface* surface, wl_output* output) {
  if (surface == nullptr || output == nullptr) {
    return;
  }

  auto it = m_surfaceOutputs.find(surface);
  if (it != m_surfaceOutputs.end()) {
    auto& outputs = it->second;
    std::erase(outputs, output);
    if (outputs.empty()) {
      m_surfaceOutputs.erase(it);
    } else if (
        auto current = m_surfaceOutputMap.find(surface);
        current != m_surfaceOutputMap.end() && current->second == output
    ) {
      current->second = outputs.back();
    }
  }

  const auto current = m_surfaceOutputMap.find(surface);
  if (current != m_surfaceOutputMap.end() && current->second == output) {
    m_surfaceOutputMap.erase(current);
  }
}

void WaylandConnection::registerLayerSurface(wl_surface* surface, zwlr_layer_surface_v1* layerSurface) {
  if (surface != nullptr && layerSurface != nullptr) {
    m_layerSurfaceMap[surface] = layerSurface;
  }
}

void WaylandConnection::unregisterSurface(wl_surface* surface) {
  if (surface != nullptr) {
    m_seatHandler.forgetSurface(surface);
    m_surfaceOutputMap.erase(surface);
    m_surfaceOutputs.erase(surface);
    m_layerSurfaceMap.erase(surface);
    if (m_lastPointerOutput != nullptr) {
      // Clear last pointer output only if it was from this surface
      // (we don't track which surface set it, so just leave it — it's a hint anyway)
    }
  }
}

zwlr_layer_surface_v1* WaylandConnection::layerSurfaceFor(wl_surface* surface) const noexcept {
  if (surface == nullptr) {
    return nullptr;
  }
  const auto it = m_layerSurfaceMap.find(surface);
  return it != m_layerSurfaceMap.end() ? it->second : nullptr;
}

void WaylandConnection::notifyOutputReady(wl_output* output) {
  if (output == nullptr || m_outputChangeCallback == nullptr) {
    return;
  }
  m_outputChangeCallback();
}

wl_output* WaylandConnection::lastPointerOutput() const noexcept { return m_lastPointerOutput; }
wl_surface* WaylandConnection::lastPointerSurface() const noexcept { return m_seatHandler.lastPointerSurface(); }
wl_surface* WaylandConnection::lastKeyboardSurface() const noexcept { return m_seatHandler.lastKeyboardSurface(); }
bool WaylandConnection::hasPointerPosition() const noexcept { return m_seatHandler.hasPointerPosition(); }
double WaylandConnection::lastPointerX() const noexcept { return m_seatHandler.lastPointerX(); }
double WaylandConnection::lastPointerY() const noexcept { return m_seatHandler.lastPointerY(); }
WaylandSeat::InputSource WaylandConnection::lastInputSource() const noexcept { return m_seatHandler.lastInputSource(); }
std::string WaylandConnection::currentKeyboardLayoutName() const { return m_seatHandler.currentLayoutName(); }
std::vector<std::string> WaylandConnection::keyboardLayoutNames() const { return m_seatHandler.layoutNames(); }
WaylandSeat::LockKeysState WaylandConnection::keyboardLockKeysState() const { return m_seatHandler.lockKeysState(); }
std::uint32_t WaylandConnection::lastInputSerial() const noexcept { return m_seatHandler.lastSerial(); }

double WaylandConnection::userIdleSeconds() const noexcept { return m_seatHandler.userIdleSeconds(); }

bool WaylandConnection::hasFreshPointerOutput(std::chrono::milliseconds maxAge) const noexcept {
  if (m_lastPointerOutput == nullptr || m_lastPointerOutputAt.time_since_epoch().count() == 0) {
    return false;
  }
  return std::chrono::steady_clock::now() - m_lastPointerOutputAt <= maxAge;
}

wl_output* WaylandConnection::outputForSurface(wl_surface* surface) const noexcept {
  if (surface == nullptr) {
    return nullptr;
  }
  const auto it = m_surfaceOutputMap.find(surface);
  return it != m_surfaceOutputMap.end() ? it->second : nullptr;
}

void WaylandConnection::setKeyboardEventCallback(WaylandSeat::KeyboardEventCallback callback) {
  m_seatHandler.setKeyboardEventCallback(std::move(callback));
}

void WaylandConnection::setLockKeysChangeCallback(WaylandSeat::LockKeysChangeCallback callback) {
  m_seatHandler.setLockKeysChangeCallback(std::move(callback));
}

void WaylandConnection::setClipboardService(ClipboardService* clipboardService) {
  m_clipboardService = clipboardService;
  bindClipboardService();
}

void WaylandConnection::setTextInputService(TextInputService* textInputService) {
  m_textInputService = textInputService;
  if (textInputService != nullptr) {
    m_seatHandler.setKeyboardFocusCallback([textInputService](wl_surface* surface, bool entered) {
      textInputService->onKeyboardFocusSurface(surface, entered);
    });
  } else {
    m_seatHandler.setKeyboardFocusCallback({});
  }
  bindTextInputService();
}

void WaylandConnection::setVirtualKeyboardService(VirtualKeyboardService* virtualKeyboardService) {
  m_virtualKeyboardService = virtualKeyboardService;
  bindVirtualKeyboardService();
}

int WaylandConnection::repeatPollTimeoutMs() const { return m_seatHandler.repeatPollTimeoutMs(); }

void WaylandConnection::repeatTick() { m_seatHandler.repeatTick(); }

void WaylandConnection::stopKeyRepeat() { m_seatHandler.stopKeyRepeat(); }

void WaylandConnection::setCursorShape(std::uint32_t serial, std::uint32_t shape) {
  m_seatHandler.setCursorShape(serial, shape);
}

std::optional<ActiveToplevel> WaylandConnection::activeToplevel() const { return m_toplevelsHandler.current(); }

std::optional<ActiveToplevel> WaylandConnection::matchToplevelByTitleAndAppId(
    std::string_view title, std::string_view appId, wl_output* preferredOutput
) const {
  return m_toplevelsHandler.matchByTitleAndAppId(title, appId, preferredOutput);
}

wl_output* WaylandConnection::activeToplevelOutput() const { return m_toplevelsHandler.currentOutput(); }

std::vector<std::string> WaylandConnection::runningAppIds(wl_output* outputFilter) const {
  if (compositors::isKde() && m_extForeignToplevels.isBound() && !m_hasForeignToplevelManagerGlobal) {
    (void)outputFilter;
    return m_extForeignToplevels.allAppIds();
  }
  return m_toplevelsHandler.allAppIds(outputFilter);
}

std::vector<ToplevelInfo> WaylandConnection::windowsForApp(
    const std::string& idLower, const std::string& wmClassLower, wl_output* outputFilter
) const {
  if (compositors::isKde() && m_extForeignToplevels.isBound() && !m_hasForeignToplevelManagerGlobal) {
    (void)outputFilter;
    return m_extForeignToplevels.windowsForApp(idLower, wmClassLower);
  }
  return m_toplevelsHandler.windowsForApp(idLower, wmClassLower, outputFilter);
}

std::vector<ToplevelInfo> WaylandConnection::windowsWithoutAppId(wl_output* outputFilter) const {
  if (compositors::isKde() && m_extForeignToplevels.isBound() && !m_hasForeignToplevelManagerGlobal) {
    (void)outputFilter;
    return m_extForeignToplevels.windowsWithoutAppId();
  }
  return m_toplevelsHandler.windowsWithoutAppId(outputFilter);
}

std::vector<ToplevelInfo>
WaylandConnection::extWindowsForApp(const std::string& idLower, const std::string& wmClassLower) const {
  if ((!compositors::isHyprland() && !compositors::isKde()) || !m_extForeignToplevels.isBound()) {
    return {};
  }
  return m_extForeignToplevels.windowsForApp(idLower, wmClassLower);
}

bool WaylandConnection::containsWlrToplevelHandle(zwlr_foreign_toplevel_handle_v1* handle) const {
  return m_toplevelsHandler.containsWlrHandle(handle);
}

void WaylandConnection::activateToplevel(zwlr_foreign_toplevel_handle_v1* handle) {
  m_toplevelsHandler.activateHandle(handle, m_seat);
}

void WaylandConnection::closeToplevel(zwlr_foreign_toplevel_handle_v1* handle) {
  m_toplevelsHandler.closeHandle(handle);
}

bool WaylandConnection::isConnected() const noexcept { return m_display != nullptr; }

bool WaylandConnection::hasRequiredGlobals() const noexcept {
  return m_compositor != nullptr && m_shm != nullptr && m_layerShell != nullptr;
}

bool WaylandConnection::hasLayerShell() const noexcept { return m_hasLayerShellGlobal; }
bool WaylandConnection::hasSubcompositor() const noexcept { return m_subcompositor != nullptr; }

bool WaylandConnection::hasXdgOutputManager() const noexcept { return m_xdgOutputManager != nullptr; }
bool WaylandConnection::hasXdgShell() const noexcept { return m_xdgWmBase != nullptr; }

bool WaylandConnection::hasExtWorkspaceManager() const noexcept { return m_hasExtWorkspaceGlobal; }
bool WaylandConnection::hasKdeVirtualDesktopManager() const noexcept { return m_hasKdeVirtualDesktopGlobal; }
bool WaylandConnection::hasDwlIpcManager() const noexcept { return m_hasDwlIpcGlobal; }
bool WaylandConnection::hasForeignToplevelManager() const noexcept { return m_hasForeignToplevelManagerGlobal; }

bool WaylandConnection::hasExtForeignToplevelList() const noexcept { return m_hasExtForeignToplevelListGlobal; }
bool WaylandConnection::hasSessionLockManager() const noexcept { return m_sessionLockManager != nullptr; }
bool WaylandConnection::hasIdleNotifier() const noexcept { return m_idleNotifier != nullptr; }
bool WaylandConnection::hasIdleInhibitManager() const noexcept { return m_idleInhibitManager != nullptr; }
bool WaylandConnection::hasXdgActivation() const noexcept { return m_xdgActivation != nullptr; }
bool WaylandConnection::hasFractionalScale() const noexcept {
  return m_fractionalScaleManager != nullptr && m_viewporter != nullptr;
}
bool WaylandConnection::hasGammaControl() const noexcept { return m_gammaControlManager != nullptr; }

bool WaylandConnection::hasScreencopy() const noexcept { return m_screencopyManager != nullptr; }

zwlr_gamma_control_manager_v1* WaylandConnection::gammaControlManager() const noexcept { return m_gammaControlManager; }

zwlr_screencopy_manager_v1* WaylandConnection::screencopyManager() const noexcept { return m_screencopyManager; }

std::string WaylandConnection::requestActivationToken(wl_surface* surface) const {
  if (m_xdgActivation == nullptr || m_display == nullptr) {
    return {};
  }

  struct TokenData {
    std::string token;
  } tokenData;

  auto* token = xdg_activation_v1_get_activation_token(m_xdgActivation);

  static const xdg_activation_token_v1_listener tokenListener = {
      .done = [](void* data, xdg_activation_token_v1* /*token*/, const char* tokenStr) {
        auto* td = static_cast<TokenData*>(data);
        td->token = tokenStr;
      },
  };

  xdg_activation_token_v1_add_listener(token, &tokenListener, &tokenData);
  xdg_activation_token_v1_set_serial(token, m_seatHandler.lastSerial(), m_seatHandler.seat());
  if (surface != nullptr) {
    xdg_activation_token_v1_set_surface(token, surface);
  }
  xdg_activation_token_v1_commit(token);
  wl_display_roundtrip(m_display);
  xdg_activation_token_v1_destroy(token);

  return tokenData.token;
}

void WaylandConnection::activateSurface(wl_surface* surface) {
  if (m_xdgActivation == nullptr || surface == nullptr) {
    return;
  }
  auto token = requestActivationToken(surface);
  if (!token.empty()) {
    xdg_activation_v1_activate(m_xdgActivation, token.c_str(), surface);
  }
}

void WaylandConnection::activateToplevelForAppId(std::string_view appId) {
  if (!hasForeignToplevelManager() || appId.empty()) {
    return;
  }
  const std::string idLower = StringUtils::toLower(std::string(appId));
  const auto windows = windowsForApp(idLower, idLower);
  if (windows.empty()) {
    return;
  }
  activateToplevel(windows.back().handle);
}

wl_display* WaylandConnection::display() const noexcept { return m_display; }

std::string WaylandConnection::describeDisplayError(int operationErrno) const {
  if (m_display == nullptr) {
    if (operationErrno != 0) {
      return std::format("display=null, operation_errno={} ({})", operationErrno, errnoText(operationErrno));
    }
    return "display=null";
  }

  const int displayError = wl_display_get_error(m_display);
  std::string detail = std::format("display_error={} ({})", displayError, errnoText(displayError));
  if (operationErrno != 0 && operationErrno != displayError) {
    detail += std::format(", operation_errno={} ({})", operationErrno, errnoText(operationErrno));
  }

  if (displayError == EMFILE || displayError == ENFILE || operationErrno == EMFILE || operationErrno == ENFILE) {
    detail += std::format(", {}", ProcessFds::describeOpenFileDescriptors());
  }

  if (displayError == EPROTO) {
    const wl_interface* interface = nullptr;
    std::uint32_t objectId = 0;
    const std::uint32_t code = wl_display_get_protocol_error(m_display, &interface, &objectId);
    const char* interfaceName = interface != nullptr && interface->name != nullptr ? interface->name : "unknown";
    detail += std::format(", protocol_error.interface={}, object_id={}, code={}", interfaceName, objectId, code);
  }

  return detail;
}

wl_compositor* WaylandConnection::compositor() const noexcept { return m_compositor; }

wl_seat* WaylandConnection::seat() const noexcept { return m_seatHandler.seat(); }

wl_shm* WaylandConnection::shm() const noexcept { return m_shm; }

wl_subcompositor* WaylandConnection::subcompositor() const noexcept { return m_subcompositor; }

zwlr_layer_shell_v1* WaylandConnection::layerShell() const noexcept { return m_layerShell; }
xdg_wm_base* WaylandConnection::xdgWmBase() const noexcept { return m_xdgWmBase; }

ext_session_lock_manager_v1* WaylandConnection::sessionLockManager() const noexcept { return m_sessionLockManager; }
ext_idle_notifier_v1* WaylandConnection::idleNotifier() const noexcept { return m_idleNotifier; }

ext_idle_notification_v1* WaylandConnection::createIdleNotification(std::uint32_t timeoutMs) const {
  wl_seat* const seat = this->seat();
  if (m_idleNotifier == nullptr || seat == nullptr) {
    return nullptr;
  }

  return ext_idle_notifier_v1_get_idle_notification(m_idleNotifier, timeoutMs, seat);
}

zwp_idle_inhibit_manager_v1* WaylandConnection::idleInhibitManager() const noexcept { return m_idleInhibitManager; }
bool WaylandConnection::hasBackgroundEffectBlur() const noexcept { return m_backgroundEffectBlurSupported; }
ext_background_effect_manager_v1* WaylandConnection::backgroundEffectManager() const noexcept {
  return m_backgroundEffectManager;
}
wp_fractional_scale_manager_v1* WaylandConnection::fractionalScaleManager() const noexcept {
  return m_fractionalScaleManager;
}

hyprland_focus_grab_manager_v1* WaylandConnection::hyprlandFocusGrabManager() const noexcept {
  return m_hyprlandFocusGrabManager;
}
FocusGrabService* WaylandConnection::focusGrabService() const noexcept { return m_focusGrabService.get(); }
wp_viewporter* WaylandConnection::viewporter() const noexcept { return m_viewporter; }

void WaylandConnection::onBackgroundEffectCapabilities(std::uint32_t capabilities) noexcept {
  m_backgroundEffectBlurSupported = (capabilities & EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR) != 0;
}

const std::vector<WaylandOutput>& WaylandConnection::outputs() const noexcept { return m_outputs; }

WaylandOutput* WaylandConnection::findOutputByWl(wl_output* wlOutput) {
  for (auto& out : m_outputs) {
    if (out.output == wlOutput) {
      return &out;
    }
  }
  return nullptr;
}

const WaylandOutput* WaylandConnection::findOutputByWl(wl_output* wlOutput) const {
  for (const auto& out : m_outputs) {
    if (out.output == wlOutput) {
      return &out;
    }
  }
  return nullptr;
}

WaylandOutput* WaylandConnection::findOutputByXdg(zxdg_output_v1* xdgOutput) {
  for (auto& out : m_outputs) {
    if (out.xdgOutput == xdgOutput) {
      return &out;
    }
  }
  return nullptr;
}

void WaylandConnection::handleGlobal(
    void* data, wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version
) {
  auto* self = static_cast<WaylandConnection*>(data);
  self->bindGlobal(registry, name, interface, version);
}

void WaylandConnection::handleGlobalRemove(void* data, wl_registry* /*registry*/, std::uint32_t name) {
  auto* self = static_cast<WaylandConnection*>(data);
  auto it = std::ranges::find_if(self->m_outputs, [name](const WaylandOutput& output) { return output.name == name; });
  if (it == self->m_outputs.end()) {
    return;
  }

  // Detach the entry from m_outputs before running any callbacks: m_outputRemovedCallback runs
  // shell code that reads m_outputs (and may re-enter Wayland dispatch), so the container must be
  // in a consistent state — never observed or mutated mid-removal.
  WaylandOutput output = std::move(*it);
  self->m_outputs.erase(it);

  if (self->m_outputRemovedCallback && output.output != nullptr) {
    self->m_outputRemovedCallback(output.output);
  }
  if (output.xdgOutput != nullptr) {
    zxdg_output_v1_destroy(output.xdgOutput);
  }
  if (output.output != nullptr) {
    if (wl_output_get_version(output.output) >= WL_OUTPUT_RELEASE_SINCE_VERSION) {
      wl_output_release(output.output);
    } else {
      wl_output_destroy(output.output);
    }
  }
  if (self->m_outputChangeCallback) {
    self->m_outputChangeCallback();
  }
}

void WaylandConnection::bindGlobal(
    wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version
) {
  const std::string interfaceName = interface;

  if (interfaceName == wl_compositor_interface.name) {
    const auto bindVersion = std::min(version, kCompositorVersion);
    m_compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, bindVersion));
    return;
  }

  if (interfaceName == wl_seat_interface.name) {
    const auto bindVersion = std::min(version, kSeatVersion);
    m_seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, bindVersion));
    m_seatHandler.bind(m_seat);
    bindClipboardService();
    bindTextInputService();
    notifyIdleCapabilitiesReady();
    return;
  }

  if (interfaceName == wl_shm_interface.name) {
    const auto bindVersion = std::min(version, kShmVersion);
    m_shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, bindVersion));
    return;
  }

  if (interfaceName == wl_subcompositor_interface.name) {
    const auto bindVersion = std::min(version, kSubcompositorVersion);
    m_subcompositor =
        static_cast<wl_subcompositor*>(wl_registry_bind(registry, name, &wl_subcompositor_interface, bindVersion));
    return;
  }

  if (interfaceName == "zwlr_layer_shell_v1") {
    m_hasLayerShellGlobal = true;
    const auto bindVersion = std::min(version, kLayerShellVersion);
    m_layerShell = static_cast<zwlr_layer_shell_v1*>(
        wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, bindVersion)
    );
    return;
  }

  if (interfaceName == zxdg_output_manager_v1_interface.name) {
    const auto bindVersion = std::min(version, kXdgOutputManagerVersion);
    m_xdgOutputManager = static_cast<zxdg_output_manager_v1*>(
        wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, bindVersion)
    );
    return;
  }

  if (interfaceName == xdg_wm_base_interface.name) {
    const auto bindVersion = std::min(version, kXdgWmBaseVersion);
    m_xdgWmBase = static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, bindVersion));
    xdg_wm_base_add_listener(m_xdgWmBase, &kXdgWmBaseListener, this);
    return;
  }

  if (interfaceName == ext_workspace_manager_v1_interface.name) {
    m_hasExtWorkspaceGlobal = true;
    const auto bindVersion = std::min(version, kExtWorkspaceManagerVersion);
    auto* manager = static_cast<ext_workspace_manager_v1*>(
        wl_registry_bind(registry, name, &ext_workspace_manager_v1_interface, bindVersion)
    );
    if (m_extWorkspaceManagerCallback) {
      m_extWorkspaceManagerCallback(manager);
    } else {
      ext_workspace_manager_v1_destroy(manager);
    }
    return;
  }

  if (interfaceName == org_kde_plasma_virtual_desktop_management_interface.name) {
    m_hasKdeVirtualDesktopGlobal = true;
    const auto bindVersion = std::min(version, kKdeVirtualDesktopManagementVersion);
    auto* manager = static_cast<org_kde_plasma_virtual_desktop_management*>(
        wl_registry_bind(registry, name, &org_kde_plasma_virtual_desktop_management_interface, bindVersion)
    );
    if (m_kdeVirtualDesktopManagerCallback) {
      m_kdeVirtualDesktopManagerCallback(manager);
    } else {
      org_kde_plasma_virtual_desktop_management_destroy(manager);
    }
    return;
  }

  if (interfaceName == zdwl_ipc_manager_v2_interface.name) {
    m_hasDwlIpcGlobal = true;
    auto* manager =
        static_cast<zdwl_ipc_manager_v2*>(wl_registry_bind(registry, name, &zdwl_ipc_manager_v2_interface, 2));
    if (m_dwlIpcManagerCallback) {
      m_dwlIpcManagerCallback(manager);
    } else {
      zdwl_ipc_manager_v2_release(manager);
    }
    return;
  }

  if (interfaceName == zwlr_foreign_toplevel_manager_v1_interface.name) {
    m_hasForeignToplevelManagerGlobal = true;
    const auto bindVersion = std::min(version, kWlrForeignToplevelManagerVersion);
    auto* manager = static_cast<zwlr_foreign_toplevel_manager_v1*>(
        wl_registry_bind(registry, name, &zwlr_foreign_toplevel_manager_v1_interface, bindVersion)
    );
    m_toplevelsHandler.bind(manager);
    return;
  }

  if (interfaceName == ext_foreign_toplevel_list_v1_interface.name) {
    const auto compositor = compositors::detect();
    // Niri/Sway also expose this global; binding it duplicates every window on top of wlr foreign-toplevel.
    if (compositor != compositors::CompositorKind::Hyprland && compositor != compositors::CompositorKind::Kde) {
      return;
    }
    m_hasExtForeignToplevelListGlobal = true;
    const auto bindVersion = std::min(version, kExtForeignToplevelListVersion);
    auto* list = static_cast<ext_foreign_toplevel_list_v1*>(
        wl_registry_bind(registry, name, &ext_foreign_toplevel_list_v1_interface, bindVersion)
    );
    m_extForeignToplevels.bind(list, m_display);
    return;
  }

  if (interfaceName == wp_cursor_shape_manager_v1_interface.name) {
    const auto bindVersion = std::min(version, kCursorShapeManagerVersion);
    m_cursorShapeManager = static_cast<wp_cursor_shape_manager_v1*>(
        wl_registry_bind(registry, name, &wp_cursor_shape_manager_v1_interface, bindVersion)
    );
    m_seatHandler.setCursorShapeManager(m_cursorShapeManager);
    return;
  }

  if (interfaceName == xdg_activation_v1_interface.name) {
    const auto bindVersion = std::min(version, kXdgActivationVersion);
    m_xdgActivation =
        static_cast<xdg_activation_v1*>(wl_registry_bind(registry, name, &xdg_activation_v1_interface, bindVersion));
    return;
  }

  if (interfaceName == ext_session_lock_manager_v1_interface.name) {
    const auto bindVersion = std::min(version, kExtSessionLockManagerVersion);
    m_sessionLockManager = static_cast<ext_session_lock_manager_v1*>(
        wl_registry_bind(registry, name, &ext_session_lock_manager_v1_interface, bindVersion)
    );
    return;
  }

  if (interfaceName == ext_idle_notifier_v1_interface.name) {
    const auto bindVersion = std::min(version, kExtIdleNotifierVersion);
    m_idleNotifier = static_cast<ext_idle_notifier_v1*>(
        wl_registry_bind(registry, name, &ext_idle_notifier_v1_interface, bindVersion)
    );
    notifyIdleCapabilitiesReady();
    return;
  }

  if (interfaceName == zwp_idle_inhibit_manager_v1_interface.name) {
    const auto bindVersion = std::min(version, kIdleInhibitManagerVersion);
    m_idleInhibitManager = static_cast<zwp_idle_inhibit_manager_v1*>(
        wl_registry_bind(registry, name, &zwp_idle_inhibit_manager_v1_interface, bindVersion)
    );
    return;
  }

  if (interfaceName == ext_background_effect_manager_v1_interface.name) {
    const auto bindVersion = std::min(version, kExtBackgroundEffectManagerVersion);
    m_backgroundEffectManager = static_cast<ext_background_effect_manager_v1*>(
        wl_registry_bind(registry, name, &ext_background_effect_manager_v1_interface, bindVersion)
    );
    ext_background_effect_manager_v1_add_listener(m_backgroundEffectManager, &kBackgroundEffectListener, this);
    return;
  }

  if (interfaceName == wp_fractional_scale_manager_v1_interface.name) {
    const auto bindVersion = std::min(version, kFractionalScaleManagerVersion);
    m_fractionalScaleManager = static_cast<wp_fractional_scale_manager_v1*>(
        wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, bindVersion)
    );
    return;
  }

  if (interfaceName == wp_viewporter_interface.name) {
    const auto bindVersion = std::min(version, kViewporterVersion);
    m_viewporter = static_cast<wp_viewporter*>(wl_registry_bind(registry, name, &wp_viewporter_interface, bindVersion));
    return;
  }

  if (interfaceName == hyprland_focus_grab_manager_v1_interface.name) {
    const auto bindVersion = std::min(version, kHyprlandFocusGrabManagerVersion);
    m_hyprlandFocusGrabManager = static_cast<hyprland_focus_grab_manager_v1*>(
        wl_registry_bind(registry, name, &hyprland_focus_grab_manager_v1_interface, bindVersion)
    );
    return;
  }

  if (interfaceName == hyprland_toplevel_mapping_manager_v1_interface.name) {
    const auto bindVersion = std::min(version, kHyprlandToplevelMappingManagerVersion);
    auto* manager = static_cast<hyprland_toplevel_mapping_manager_v1*>(
        wl_registry_bind(registry, name, &hyprland_toplevel_mapping_manager_v1_interface, bindVersion)
    );
    if (m_hyprlandToplevelMappingManagerCallback) {
      m_hyprlandToplevelMappingManagerCallback(manager);
    } else {
      hyprland_toplevel_mapping_manager_v1_destroy(manager);
    }
    return;
  }

  if (interfaceName == ext_data_control_manager_v1_interface.name) {
    if (m_dataControlManager != nullptr && m_dataControlOps != extDataControlOps()) {
      m_dataControlOps->destroyManager(m_dataControlManager);
      m_dataControlManager = nullptr;
    }
    if (m_dataControlManager == nullptr) {
      m_dataControlManager = extDataControlOps()->bindManager(registry, name, version);
      m_dataControlOps = extDataControlOps();
      bindClipboardService();
    }
    return;
  }

  if (interfaceName == zwlr_data_control_manager_v1_interface.name) {
    if (m_dataControlManager == nullptr) {
      m_dataControlManager = wlrDataControlOps()->bindManager(registry, name, version);
      m_dataControlOps = wlrDataControlOps();
      bindClipboardService();
    }
    return;
  }

  if (interfaceName == zwp_text_input_manager_v3_interface.name) {
    const auto bindVersion = std::min(version, kTextInputManagerVersion);
    m_textInputManager = static_cast<zwp_text_input_manager_v3*>(
        wl_registry_bind(registry, name, &zwp_text_input_manager_v3_interface, bindVersion)
    );
    bindTextInputService();
    return;
  }

  if (interfaceName == zwp_virtual_keyboard_manager_v1_interface.name) {
    const auto bindVersion = std::min(version, kVirtualKeyboardManagerVersion);
    m_virtualKeyboardManager = static_cast<zwp_virtual_keyboard_manager_v1*>(
        wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, bindVersion)
    );
    bindVirtualKeyboardService();
    return;
  }

  if (interfaceName == zwlr_gamma_control_manager_v1_interface.name) {
    const auto bindVersion = std::min(version, kGammaControlManagerVersion);
    m_gammaControlManager = static_cast<zwlr_gamma_control_manager_v1*>(
        wl_registry_bind(registry, name, &zwlr_gamma_control_manager_v1_interface, bindVersion)
    );
    return;
  }

  if (interfaceName == zwlr_screencopy_manager_v1_interface.name) {
    const auto bindVersion = std::min(version, kScreencopyManagerVersion);
    m_screencopyManager = static_cast<zwlr_screencopy_manager_v1*>(
        wl_registry_bind(registry, name, &zwlr_screencopy_manager_v1_interface, bindVersion)
    );
    return;
  }

  if (interfaceName == wl_output_interface.name) {
    const auto bindVersion = std::min(version, kOutputVersion);
    auto* output = static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, bindVersion));
    m_outputs.push_back(
        WaylandOutput{
            .name = name,
            .interfaceName = interfaceName,
            .connectorName = {},
            .description = {},
            .version = version,
            .output = output,
        }
    );
    wl_output_add_listener(output, &kOutputListener, this);
    if (m_xdgOutputManager != nullptr) {
      auto* xdgOut = zxdg_output_manager_v1_get_xdg_output(m_xdgOutputManager, output);
      m_outputs.back().xdgOutput = xdgOut;
      zxdg_output_v1_add_listener(xdgOut, &kXdgOutputListener, this);
    }
    // Notify the shell only after m_outputs is fully populated for this output: the callback runs
    // shell code that may mutate m_outputs, which would invalidate the back() access above.
    if (m_outputAddedCallback) {
      m_outputAddedCallback(output);
    }
  }
}

void WaylandConnection::bindClipboardService() {
  if (m_clipboardService == nullptr) {
    return;
  }
  if (m_dataControlManager == nullptr || m_dataControlOps == nullptr || m_seat == nullptr) {
    return;
  }
  m_clipboardService->bind(m_dataControlManager, m_dataControlOps, m_seat);
}

void WaylandConnection::bindTextInputService() {
  if (m_textInputService == nullptr) {
    return;
  }
  if (m_textInputManager == nullptr || m_seat == nullptr) {
    return;
  }
  m_textInputService->bind(m_textInputManager, m_seat);
}

void WaylandConnection::bindVirtualKeyboardService() {
  if (m_virtualKeyboardService == nullptr) {
    return;
  }
  if (m_virtualKeyboardManager == nullptr || m_seat == nullptr) {
    return;
  }
  m_virtualKeyboardService->bind(m_virtualKeyboardManager, m_seat);
}

void WaylandConnection::cleanup() {
  if (m_clipboardService != nullptr) {
    m_clipboardService->cleanup();
  }
  if (m_textInputService != nullptr) {
    m_textInputService->cleanup();
  }
  if (m_virtualKeyboardService != nullptr) {
    m_virtualKeyboardService->cleanup();
  }
  m_toplevelsHandler.cleanup();
  m_extForeignToplevels.cleanup();

  for (auto& out : m_outputs) {
    if (out.xdgOutput != nullptr) {
      zxdg_output_v1_destroy(out.xdgOutput);
      out.xdgOutput = nullptr;
    }
  }

  if (m_xdgOutputManager != nullptr) {
    zxdg_output_manager_v1_destroy(m_xdgOutputManager);
    m_xdgOutputManager = nullptr;
  }

  if (m_xdgWmBase != nullptr) {
    xdg_wm_base_destroy(m_xdgWmBase);
    m_xdgWmBase = nullptr;
  }

  if (m_layerShell != nullptr) {
    zwlr_layer_shell_v1_destroy(m_layerShell);
    m_layerShell = nullptr;
  }

  m_seatHandler.cleanup();

  if (m_xdgActivation != nullptr) {
    xdg_activation_v1_destroy(m_xdgActivation);
    m_xdgActivation = nullptr;
  }

  if (m_sessionLockManager != nullptr) {
    ext_session_lock_manager_v1_destroy(m_sessionLockManager);
    m_sessionLockManager = nullptr;
  }
  if (m_idleNotifier != nullptr) {
    ext_idle_notifier_v1_destroy(m_idleNotifier);
    m_idleNotifier = nullptr;
  }
  if (m_idleInhibitManager != nullptr) {
    zwp_idle_inhibit_manager_v1_destroy(m_idleInhibitManager);
    m_idleInhibitManager = nullptr;
  }
  if (m_backgroundEffectManager != nullptr) {
    ext_background_effect_manager_v1_destroy(m_backgroundEffectManager);
    m_backgroundEffectManager = nullptr;
    m_backgroundEffectBlurSupported = false;
  }

  if (m_fractionalScaleManager != nullptr) {
    wp_fractional_scale_manager_v1_destroy(m_fractionalScaleManager);
    m_fractionalScaleManager = nullptr;
  }

  if (m_hyprlandFocusGrabManager != nullptr) {
    hyprland_focus_grab_manager_v1_destroy(m_hyprlandFocusGrabManager);
    m_hyprlandFocusGrabManager = nullptr;
  }

  if (m_gammaControlManager != nullptr) {
    zwlr_gamma_control_manager_v1_destroy(m_gammaControlManager);
    m_gammaControlManager = nullptr;
  }
  if (m_screencopyManager != nullptr) {
    zwlr_screencopy_manager_v1_destroy(m_screencopyManager);
    m_screencopyManager = nullptr;
  }

  if (m_viewporter != nullptr) {
    wp_viewporter_destroy(m_viewporter);
    m_viewporter = nullptr;
  }

  if (m_dataControlManager != nullptr && m_dataControlOps != nullptr) {
    m_dataControlOps->destroyManager(m_dataControlManager);
    m_dataControlManager = nullptr;
    m_dataControlOps = nullptr;
  }

  if (m_virtualKeyboardManager != nullptr) {
    zwp_virtual_keyboard_manager_v1_destroy(m_virtualKeyboardManager);
    m_virtualKeyboardManager = nullptr;
  }

  if (m_textInputManager != nullptr) {
    zwp_text_input_manager_v3_destroy(m_textInputManager);
    m_textInputManager = nullptr;
  }

  if (m_cursorShapeManager != nullptr) {
    wp_cursor_shape_manager_v1_destroy(m_cursorShapeManager);
    m_cursorShapeManager = nullptr;
  }

  if (m_seat != nullptr) {
    wl_seat_destroy(m_seat);
    m_seat = nullptr;
  }

  if (m_shm != nullptr) {
    wl_shm_destroy(m_shm);
    m_shm = nullptr;
  }

  if (m_subcompositor != nullptr) {
    wl_subcompositor_destroy(m_subcompositor);
    m_subcompositor = nullptr;
  }

  if (m_compositor != nullptr) {
    wl_compositor_destroy(m_compositor);
    m_compositor = nullptr;
  }

  for (auto& output : m_outputs) {
    if (output.output != nullptr) {
      if (m_outputRemovedCallback) {
        m_outputRemovedCallback(output.output);
      }
      wl_output_destroy(output.output);
      output.output = nullptr;
    }
  }

  if (m_registry != nullptr) {
    wl_registry_destroy(m_registry);
    m_registry = nullptr;
  }

  if (m_display != nullptr) {
    wl_display_disconnect(m_display);
    m_display = nullptr;
  }

  m_outputs.clear();
  m_surfaceOutputMap.clear();
  m_surfaceOutputs.clear();
  m_layerSurfaceMap.clear();
  m_hasLayerShellGlobal = false;
  m_hasExtWorkspaceGlobal = false;
  m_hasDwlIpcGlobal = false;
  m_hasForeignToplevelManagerGlobal = false;
  m_outputAddedCallback = nullptr;
  m_outputRemovedCallback = nullptr;
  m_extWorkspaceManagerCallback = nullptr;
  m_kdeVirtualDesktopManagerCallback = nullptr;
  m_dwlIpcManagerCallback = nullptr;
}

void WaylandConnection::logStartupSummary() const {
  kLog.info(
      "connected compositor={} shm={} layer-shell={} xdg-shell={} xdg-output={} ext-workspace={} kde-vd={} dwl-ipc={} "
      "session-lock={} fractional-scale={} gamma-control={} outputs={}",
      m_compositor != nullptr ? "yes" : "no", m_shm != nullptr ? "yes" : "no", hasLayerShell() ? "yes" : "no",
      hasXdgShell() ? "yes" : "no", hasXdgOutputManager() ? "yes" : "no", hasExtWorkspaceManager() ? "yes" : "no",
      hasKdeVirtualDesktopManager() ? "yes" : "no", hasDwlIpcManager() ? "yes" : "no",
      hasSessionLockManager() ? "yes" : "no", hasFractionalScale() ? "yes" : "no", hasGammaControl() ? "yes" : "no",
      m_outputs.size()
  );

  for (const auto& output : m_outputs) {
    const DetectedOutputScale detectedScale = detectOutputScale(output);
    if (detectedScale.available) {
      kLog.info(
          "output {} global={} wl_scale={} detected_fractional_scale={:.3f} logical={}x{} mode={}x{} orientation={} "
          "desc=\"{}\"",
          outputLabel(output), output.name, output.scale, detectedScale.scale, output.logicalWidth,
          output.logicalHeight, output.width, output.height, detectedScale.rotated ? "rotated" : "normal",
          output.description
      );
    } else {
      kLog.info(
          "output {} global={} wl_scale={} detected_fractional_scale=unavailable logical={}x{} mode={}x{} desc=\"{}\"",
          outputLabel(output), output.name, output.scale, output.logicalWidth, output.logicalHeight, output.width,
          output.height, output.description
      );
    }
  }
}
