#include "application.h"
#include "application_internal.h"
#include "core/log.h"
#include "dbus/bluetooth/bluetooth_service.h"
#include "dbus/network/inetwork_service.h"
#include "render/backend/render_backend.h"

#include <exception>

namespace {
  constexpr Logger kLog("app");

  std::string_view powerProfileOriginName(PowerProfilesChangeOrigin origin) {
    switch (origin) {
    case PowerProfilesChangeOrigin::Noctalia:
      return "noctalia";
    case PowerProfilesChangeOrigin::External:
      return "external";
    }
    return "external";
  }
} // namespace

void Application::onIconThemeChanged() {
  kLog.info("system icon theme changed; refreshing icon consumers");
  m_bar.reload();
  m_dock.reload();
  m_panelManager.onIconThemeChanged();
  m_notificationToast.requestLayout();
}

void Application::onGraphicsReset(RenderGraphicsResetStatus status) {
  (void)status;
  // Backstop: a GPU-resource rebuild right after a context loss can still hit a not-yet-ready
  // context. Never let an exception escape — this runs inside renderScene, itself reached from a
  // libffi-dispatched Wayland listener, where a throw would abort. The dirty-flag/generation
  // machinery retries the rebuild on the next frame.
  try {
    m_sharedTextureCache.reloadResidentTextures();
    m_asyncTextureCache.reloadResidentTextures();
    m_thumbnailService.invalidateGpuResources(m_renderContext.backend().textureManager());
    m_wallpaper.onGpuResourcesInvalidated();
    m_backdrop.onGpuResourcesInvalidated();
    m_lockScreen.onGpuResourcesInvalidated();
    m_trayMenu.requestLayout();
    m_settingsWindow.requestRedraw();
    m_screenCorners.requestRedraw();
    requestAllSurfacesRedraw();
  } catch (const std::exception& e) {
    kLog.warn("graphics-reset recovery deferred: {}", e.what());
  }
}

void Application::requestAllSurfacesRedraw() {
  m_bar.requestRedraw();
  m_dock.requestRedraw();
  m_desktopWidgetsController.requestRedraw();
  m_panelManager.requestRedraw();
  m_notificationToast.requestRedraw();
  m_osdOverlay.requestRedraw();
  m_colorPickerDialogPopup.requestRedraw();
  m_glyphPickerDialogPopup.requestRedraw();
  m_fileDialogPopup.requestRedraw();
}

void Application::onUpowerStateChangedForHooks() {
  if (m_upowerService == nullptr) {
    return;
  }
  for (const auto& event : m_batteryHookState.update(m_upowerService->state())) {
    if (event.env.empty()) {
      m_hookManager.fire(event.kind);
    } else {
      m_hookManager.fire(event.kind, event.env);
    }
  }
}

void Application::onNetworkStateChangedForEvents(const NetworkState& state, NetworkChangeOrigin origin) {
  if (!m_prevWirelessEnabledForEvents.has_value()) {
    m_prevWirelessEnabledForEvents = state.wirelessEnabled;
    return;
  }
  const bool prev = *m_prevWirelessEnabledForEvents;
  if (prev != state.wirelessEnabled) {
    if (origin != NetworkChangeOrigin::Noctalia) {
      m_osdOverlay.show(wifiOsdContent(state.wirelessEnabled));
    }
    if (state.wirelessEnabled) {
      m_hookManager.fire(HookKind::WifiEnabled);
    } else {
      m_hookManager.fire(HookKind::WifiDisabled);
    }
  }
  m_prevWirelessEnabledForEvents = state.wirelessEnabled;
}

void Application::onBluetoothStateChangedForEvents(const BluetoothState& state, BluetoothStateChangeOrigin origin) {
  if (!m_prevBluetoothPoweredForEvents.has_value()) {
    m_prevBluetoothPoweredForEvents = state.powered;
    return;
  }
  const bool prev = *m_prevBluetoothPoweredForEvents;
  if (prev != state.powered) {
    if (origin != BluetoothStateChangeOrigin::Noctalia) {
      m_osdOverlay.show(bluetoothOsdContent(state.powered));
    }
    if (state.powered) {
      m_hookManager.fire(HookKind::BluetoothEnabled);
    } else {
      m_hookManager.fire(HookKind::BluetoothDisabled);
    }
  }
  m_prevBluetoothPoweredForEvents = state.powered;
}

void Application::onPowerProfileChangedForEvents(const PowerProfilesState& state, PowerProfilesChangeOrigin origin) {
  if (state.activeProfile.empty()) {
    return;
  }
  if (!m_prevPowerProfileActiveForEvents.has_value()) {
    m_prevPowerProfileActiveForEvents = state.activeProfile;
    return;
  }
  const std::string prev = *m_prevPowerProfileActiveForEvents;
  if (prev != state.activeProfile) {
    if (origin != PowerProfilesChangeOrigin::Noctalia) {
      m_osdOverlay.show(powerProfileOsdContent(state.activeProfile));
    }
    m_hookManager.fire(
        HookKind::PowerProfileChanged,
        {{"NOCTALIA_POWER_PROFILE", state.activeProfile},
         {"NOCTALIA_POWER_PROFILE_PREVIOUS", prev},
         {"NOCTALIA_POWER_PROFILE_ORIGIN", std::string(powerProfileOriginName(origin))}}
    );
  }
  m_prevPowerProfileActiveForEvents = state.activeProfile;
}
