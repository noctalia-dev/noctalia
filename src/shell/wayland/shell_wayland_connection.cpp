#include "compositors/compositor_detect.h"
#include "dwl-ipc-unstable-v2-client-protocol.h"
#include "ext-background-effect-v1-client-protocol.h"
#include "ext-data-control-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-idle-notify-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "ext-workspace-v1-client-protocol.h"
#include "hyprland-focus-grab-v1-client-protocol.h"
#include "hyprland-toplevel-mapping-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "org-kde-plasma-virtual-desktop-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wayland/clipboard_service.h"
#include "wayland/hyprland/focus_grab_service.h"
#include "wayland/virtual_keyboard_service.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_protocol_policy.h"
#include "wlr-data-control-unstable-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "wlr-gamma-control-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-output-management-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"

#include <algorithm>
#include <utility>

namespace {
  constexpr std::uint32_t kExtWorkspaceManagerVersion = 1;
  constexpr std::uint32_t kKdeVirtualDesktopManagementVersion = 4;
  constexpr std::uint32_t kWlrForeignToplevelManagerVersion = 3;
  constexpr std::uint32_t kExtForeignToplevelListVersion = 1;
  constexpr std::uint32_t kExtSessionLockManagerVersion = 1;
  constexpr std::uint32_t kExtIdleNotifierVersion = 2;
  constexpr std::uint32_t kIdleInhibitManagerVersion = 1;
  constexpr std::uint32_t kHyprlandFocusGrabManagerVersion = 1;
  constexpr std::uint32_t kHyprlandToplevelMappingManagerVersion = 1;
  constexpr std::uint32_t kVirtualKeyboardManagerVersion = 1;
  constexpr std::uint32_t kGammaControlManagerVersion = 1;
  constexpr std::uint32_t kScreencopyManagerVersion = 3;
  constexpr std::uint32_t kImageCopyCaptureManagerVersion = 1;
  constexpr std::uint32_t kOutputImageCaptureSourceManagerVersion = 1;
  constexpr std::uint32_t kForeignToplevelImageCaptureSourceManagerVersion = 1;
  constexpr std::uint32_t kOutputManagerVersion = 4;
  constexpr std::uint32_t kOutputManagerMinVersion = 3;
  constexpr std::uint32_t kExtBackgroundEffectManagerVersion = 1;
  // REMOVEME(wayland-protocols-1.45): Use the generated mask after requiring wayland-protocols >= 1.46;
  // version 1.45 generates zero for blur although the corrected v1 wire mask is 1.
  constexpr std::uint32_t kExtBackgroundEffectBlurCapabilityMask = 1U;
  constexpr std::uint32_t kLayerShellVersion = 4;

  void
  backgroundEffectCapabilities(void* data, ext_background_effect_manager_v1* /*manager*/, std::uint32_t capabilities) {
    auto* self = static_cast<WaylandConnection*>(data);
    self->onBackgroundEffectCapabilities(capabilities);
  }

  const ext_background_effect_manager_v1_listener kBackgroundEffectListener = {
      .capabilities = &backgroundEffectCapabilities,
  };

  void outputHeadName(void* data, zwlr_output_head_v1* head, const char* name) {
    static_cast<WaylandConnection*>(data)->onOutputHeadName(head, name);
  }

  void outputHeadMake(void* data, zwlr_output_head_v1* head, const char* make) {
    static_cast<WaylandConnection*>(data)->onOutputHeadMake(head, make);
  }

  void outputHeadModel(void* data, zwlr_output_head_v1* head, const char* model) {
    static_cast<WaylandConnection*>(data)->onOutputHeadModel(head, model);
  }

  void outputHeadSerialNumber(void* data, zwlr_output_head_v1* head, const char* serialNumber) {
    static_cast<WaylandConnection*>(data)->onOutputHeadSerialNumber(head, serialNumber);
  }

  void outputModeFinished(void* data, zwlr_output_mode_v1* mode) {
    static_cast<WaylandConnection*>(data)->onOutputModeFinished(mode);
  }

  const zwlr_output_mode_v1_listener kOutputModeListener = {
      .size = [](void*, zwlr_output_mode_v1*, int32_t, int32_t) {},
      .refresh = [](void*, zwlr_output_mode_v1*, int32_t) {},
      .preferred = [](void*, zwlr_output_mode_v1*) {},
      .finished = outputModeFinished,
  };

  void outputHeadMode(void* data, zwlr_output_head_v1* head, zwlr_output_mode_v1* mode) {
    static_cast<WaylandConnection*>(data)->onOutputHeadMode(head, mode);
  }

  void outputHeadFinished(void* data, zwlr_output_head_v1* head) {
    static_cast<WaylandConnection*>(data)->onOutputHeadFinished(head);
  }

  void outputHeadScale(void* data, zwlr_output_head_v1* head, wl_fixed_t scale) {
    static_cast<WaylandConnection*>(data)->onOutputHeadScale(head, wl_fixed_to_double(scale));
  }

  void outputHeadTransform(void* data, zwlr_output_head_v1* head, int32_t transform) {
    static_cast<WaylandConnection*>(data)->onOutputHeadTransform(head, transform);
  }

  // libwayland aborts on an event with a null listener slot, even one we don't need.
  const zwlr_output_head_v1_listener kOutputHeadListener = {
      .name = outputHeadName,
      .description = [](void*, zwlr_output_head_v1*, const char*) {},
      .physical_size = [](void*, zwlr_output_head_v1*, int32_t, int32_t) {},
      .mode = outputHeadMode,
      .enabled = [](void*, zwlr_output_head_v1*, int32_t) {},
      .current_mode = [](void*, zwlr_output_head_v1*, zwlr_output_mode_v1*) {},
      .position = [](void*, zwlr_output_head_v1*, int32_t, int32_t) {},
      .transform = outputHeadTransform,
      .scale = outputHeadScale,
      .finished = outputHeadFinished,
      .make = outputHeadMake,
      .model = outputHeadModel,
      .serial_number = outputHeadSerialNumber,
      .adaptive_sync = [](void*, zwlr_output_head_v1*, uint32_t) {},
  };

  void outputManagerHead(void* data, zwlr_output_manager_v1* /*manager*/, zwlr_output_head_v1* head) {
    auto* self = static_cast<WaylandConnection*>(data);
    self->onOutputManagerHead(head);
    zwlr_output_head_v1_add_listener(head, &kOutputHeadListener, data);
  }

  void outputManagerDone(void* data, zwlr_output_manager_v1* /*manager*/, std::uint32_t /*serial*/) {
    static_cast<WaylandConnection*>(data)->onOutputManagerDone();
  }

  void outputManagerFinished(void* data, zwlr_output_manager_v1* manager) {
    static_cast<WaylandConnection*>(data)->onOutputManagerFinished(manager);
  }

  const zwlr_output_manager_v1_listener kOutputManagerListener = {
      .head = outputManagerHead,
      .done = outputManagerDone,
      .finished = outputManagerFinished,
  };

} // namespace

WaylandConnection::WaylandConnection(ShellProtocols, Purpose purpose)
    : m_purpose(purpose), m_extension(&kShellExtension) {}

const WaylandConnection::Extension WaylandConnection::kShellExtension{
    &WaylandConnection::bindShellGlobal,
    &WaylandConnection::bindClipboardService,
    &WaylandConnection::shellConnected,
    &WaylandConnection::cleanupShell,
};

void WaylandConnection::shellConnected() {
  m_focusGrabService = std::make_unique<FocusGrabService>();
  m_focusGrabService->initialize(m_hyprlandFocusGrabManager);
  logStartupSummary();
}

void WaylandConnection::setClipboardService(ClipboardService* clipboardService) {
  m_clipboardService = clipboardService;
  bindClipboardService();
}

void WaylandConnection::setVirtualKeyboardService(VirtualKeyboardService* virtualKeyboardService) {
  m_virtualKeyboardService = virtualKeyboardService;
  bindVirtualKeyboardService();
}

void WaylandConnection::setToplevelChangeCallback(ChangeCallback callback) {
  m_toplevelsHandler.setChangeCallback(callback);
  m_extForeignToplevels.setChangeCallback(std::move(callback));
}

std::optional<ActiveToplevel> WaylandConnection::activeToplevel() const { return m_toplevelsHandler.current(); }

std::optional<ActiveToplevel> WaylandConnection::matchToplevelByTitleAndAppId(
    std::string_view title, std::string_view appId, wl_output* preferredOutput
) const {
  return m_toplevelsHandler.matchByTitleAndAppId(title, appId, preferredOutput);
}

wl_output* WaylandConnection::activeToplevelOutput() const { return m_toplevelsHandler.currentOutput(); }

bool WaylandConnection::hasFullscreenToplevel() const { return m_toplevelsHandler.anyFullscreen(); }

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
  if (!m_extForeignToplevels.isBound()) {
    return {};
  }
  return m_extForeignToplevels.windowsForApp(idLower, wmClassLower);
}

std::vector<ToplevelInfo> WaylandConnection::extWindowsWithoutAppId() const {
  return m_extForeignToplevels.isBound() ? m_extForeignToplevels.windowsWithoutAppId() : std::vector<ToplevelInfo>{};
}

ext_foreign_toplevel_handle_v1* WaylandConnection::extHandleForIdentifier(std::string_view identifier) const {
  return m_extForeignToplevels.isBound() ? m_extForeignToplevels.extHandleForIdentifier(identifier) : nullptr;
}

bool WaylandConnection::containsWlrToplevelHandle(zwlr_foreign_toplevel_handle_v1* handle) const {
  return m_toplevelsHandler.containsWlrHandle(handle);
}

void WaylandConnection::activateToplevel(zwlr_foreign_toplevel_handle_v1* handle) {
  m_toplevelsHandler.activateHandle(handle, m_seat);
}

bool WaylandConnection::minimizeToplevel(zwlr_foreign_toplevel_handle_v1* handle) {
  return m_toplevelsHandler.minimizeHandle(handle);
}

std::optional<bool> WaylandConnection::toplevelMinimized(zwlr_foreign_toplevel_handle_v1* handle) const {
  return m_toplevelsHandler.minimized(handle);
}

void WaylandConnection::closeToplevel(zwlr_foreign_toplevel_handle_v1* handle) {
  m_toplevelsHandler.closeHandle(handle);
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

void WaylandConnection::bindVirtualKeyboardService() {
  if (m_virtualKeyboardService == nullptr) {
    return;
  }
  if (m_virtualKeyboardManager == nullptr || m_seat == nullptr) {
    return;
  }
  m_virtualKeyboardService->bind(m_virtualKeyboardManager, m_seat);
}

void WaylandConnection::onBackgroundEffectCapabilities(std::uint32_t capabilities) noexcept {
  m_backgroundEffectBlurSupported = (capabilities & kExtBackgroundEffectBlurCapabilityMask) != 0U;
}

void WaylandConnection::onOutputHeadMode(zwlr_output_head_v1* /*head*/, zwlr_output_mode_v1* mode) {
  if (mode == nullptr) {
    return;
  }
  m_outputModes.insert(mode);
  zwlr_output_mode_v1_add_listener(mode, &kOutputModeListener, this);
}

void WaylandConnection::onOutputModeFinished(zwlr_output_mode_v1* mode) {
  if (m_outputModes.erase(mode) > 0) {
    zwlr_output_mode_v1_release(mode);
  }
}

void WaylandConnection::onOutputHeadFinished(zwlr_output_head_v1* head) {
  m_outputHeads.erase(head);
  zwlr_output_head_v1_release(head);
}

void WaylandConnection::onOutputManagerDone() { matchPendingOutputHeads(); }

void WaylandConnection::onOutputManagerFinished(zwlr_output_manager_v1* manager) {
  for (auto* mode : m_outputModes) {
    zwlr_output_mode_v1_release(mode);
  }
  m_outputModes.clear();
  for (const auto& entry : m_outputHeads) {
    zwlr_output_head_v1_release(entry.first);
  }
  m_outputHeads.clear();
  zwlr_output_manager_v1_destroy(manager);
  m_outputManager = nullptr;
}

ext_idle_notification_v1* WaylandConnection::createIdleNotification(std::uint32_t timeoutMs) const {
  wl_seat* const seat = this->seat();
  if (m_idleNotifier == nullptr || seat == nullptr) {
    return nullptr;
  }

  return ext_idle_notifier_v1_get_idle_notification(m_idleNotifier, timeoutMs, seat);
}

ext_idle_notification_v1* WaylandConnection::createInputIdleNotification(std::uint32_t timeoutMs) const {
  wl_seat* const seat = this->seat();
  if (m_idleNotifier == nullptr
      || seat == nullptr
      || ext_idle_notifier_v1_get_version(m_idleNotifier)
          < EXT_IDLE_NOTIFIER_V1_GET_INPUT_IDLE_NOTIFICATION_SINCE_VERSION) {
    return nullptr;
  }

  return ext_idle_notifier_v1_get_input_idle_notification(m_idleNotifier, timeoutMs, seat);
}

void WaylandConnection::bindShellGlobal(
    wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version
) {
  const std::string_view interfaceName = interface;

  if (m_purpose == Purpose::Screencopy && interfaceName != zwlr_screencopy_manager_v1_interface.name) {
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
    // ext handles carry the exact identifier joined with Niri/Umbriel IPC window ids and are the source for toplevel
    // image capture. Keep the wlr manager bound as well because other shell features still consume its richer state.
    if (!wayland_protocol_policy::shouldBindExtForeignToplevelList(compositor)) {
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

  if (interfaceName == ext_image_copy_capture_manager_v1_interface.name) {
    const auto bindVersion = std::min(version, kImageCopyCaptureManagerVersion);
    m_imageCopyCaptureManager = static_cast<ext_image_copy_capture_manager_v1*>(
        wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, bindVersion)
    );
    return;
  }

  if (interfaceName == ext_output_image_capture_source_manager_v1_interface.name) {
    const auto bindVersion = std::min(version, kOutputImageCaptureSourceManagerVersion);
    m_outputImageCaptureSourceManager = static_cast<ext_output_image_capture_source_manager_v1*>(
        wl_registry_bind(registry, name, &ext_output_image_capture_source_manager_v1_interface, bindVersion)
    );
    return;
  }

  if (interfaceName == ext_foreign_toplevel_image_capture_source_manager_v1_interface.name) {
    const auto bindVersion = std::min(version, kForeignToplevelImageCaptureSourceManagerVersion);
    m_foreignToplevelImageCaptureSourceManager = static_cast<ext_foreign_toplevel_image_capture_source_manager_v1*>(
        wl_registry_bind(registry, name, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, bindVersion)
    );
    return;
  }

  if (interfaceName == zwlr_output_manager_v1_interface.name) {
    // head/mode release requests need v3; nothing useful to bind below that anyway.
    if (version < kOutputManagerMinVersion) {
      return;
    }
    const auto bindVersion = std::min(version, kOutputManagerVersion);
    m_outputManager = static_cast<zwlr_output_manager_v1*>(
        wl_registry_bind(registry, name, &zwlr_output_manager_v1_interface, bindVersion)
    );
    zwlr_output_manager_v1_add_listener(m_outputManager, &kOutputManagerListener, this);
    return;
  }
}

void WaylandConnection::cleanupShell() {
  if (m_clipboardService != nullptr) {
    m_clipboardService->cleanup();
  }

  if (m_virtualKeyboardService != nullptr) {
    m_virtualKeyboardService->cleanup();
  }

  m_toplevelsHandler.cleanup();
  m_extForeignToplevels.cleanup();
  m_focusGrabService.reset();

  if (m_layerShell != nullptr) {
    zwlr_layer_shell_v1_destroy(m_layerShell);
    m_layerShell = nullptr;
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

  if (m_imageCopyCaptureManager != nullptr) {
    ext_image_copy_capture_manager_v1_destroy(m_imageCopyCaptureManager);
    m_imageCopyCaptureManager = nullptr;
  }

  if (m_outputImageCaptureSourceManager != nullptr) {
    ext_output_image_capture_source_manager_v1_destroy(m_outputImageCaptureSourceManager);
    m_outputImageCaptureSourceManager = nullptr;
  }

  if (m_foreignToplevelImageCaptureSourceManager != nullptr) {
    ext_foreign_toplevel_image_capture_source_manager_v1_destroy(m_foreignToplevelImageCaptureSourceManager);
    m_foreignToplevelImageCaptureSourceManager = nullptr;
  }

  for (auto* mode : m_outputModes) {
    zwlr_output_mode_v1_release(mode);
  }
  m_outputModes.clear();
  for (const auto& entry : m_outputHeads) {
    zwlr_output_head_v1_release(entry.first);
  }
  m_outputHeads.clear();
  if (m_outputManager != nullptr) {
    zwlr_output_manager_v1_destroy(m_outputManager);
    m_outputManager = nullptr;
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
}
