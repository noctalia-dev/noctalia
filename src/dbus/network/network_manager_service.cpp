#include "dbus/network/network_manager_service.h"

#include "core/log.h"
#include "dbus/system_bus.h"
#include "system/rfkill_helper.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <set>
#include <vector>

namespace {

  constexpr Logger kLog("network");

  const sdbus::ServiceName kNmBusName{"org.freedesktop.NetworkManager"};
  const sdbus::ObjectPath kNmObjectPath{"/org/freedesktop/NetworkManager"};
  constexpr auto kNmInterface = "org.freedesktop.NetworkManager";
  constexpr auto kNmDeviceInterface = "org.freedesktop.NetworkManager.Device";
  constexpr auto kNmDeviceWirelessInterface = "org.freedesktop.NetworkManager.Device.Wireless";
  constexpr auto kNmSettingsInterface = "org.freedesktop.NetworkManager.Settings";
  const sdbus::ObjectPath kNmSettingsObjectPath{"/org/freedesktop/NetworkManager/Settings"};
  constexpr auto kNmSettingsConnectionInterface = "org.freedesktop.NetworkManager.Settings.Connection";

  // NM80211ApSecurityFlags bits we care about.
  constexpr std::uint32_t k_nm80211ApSecNone = 0x0;
  constexpr auto kNmActiveConnectionInterface = "org.freedesktop.NetworkManager.Connection.Active";
  constexpr auto kNmAccessPointInterface = "org.freedesktop.NetworkManager.AccessPoint";
  constexpr auto k_nmIp4ConfigInterface = "org.freedesktop.NetworkManager.IP4Config";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";

  using ConnectionSettings = std::map<std::string, std::map<std::string, sdbus::Variant>>;
  using VariantMap = std::map<std::string, sdbus::Variant>;

  // NMDeviceType values from NetworkManager D-Bus API.
  constexpr std::uint32_t kNmDeviceTypeWifi = 2;

  // NMActiveConnectionState
  constexpr std::uint32_t kNmActiveConnectionStateActivating = 1;
  constexpr std::uint32_t kNmActiveConnectionStateActivated = 2;
  constexpr std::uint32_t kNmActiveConnectionStateDeactivated = 4;

  // NMSettingsConnectionFlags / NMSettingsUpdate2Flags.
  constexpr std::uint32_t kNmSettingsConnectionFlagUnsaved = 0x01;
  constexpr std::uint32_t k_nmSettingsUpdate2FlagToDisk = 0x01;

  template <typename T>
  T getPropertyOr(sdbus::IProxy& proxy, std::string_view interfaceName, std::string_view propertyName, T fallback) {
    try {
      const sdbus::Variant value = proxy.getProperty(propertyName).onInterface(std::string(interfaceName));
      return value.get<T>();
    } catch (const sdbus::Error&) {
      return fallback;
    }
  }

  std::string ipv4FromUint(std::uint32_t addrLe) {
    // NM stores IPv4 addresses as native-byte-order uint32 in network order bytes.
    // I.e. the bytes a.b.c.d are laid out in memory low->high as a,b,c,d.
    std::array<std::uint8_t, 4> bytes{};
    bytes[0] = static_cast<std::uint8_t>(addrLe & 0xffU);
    bytes[1] = static_cast<std::uint8_t>((addrLe >> 8) & 0xffU);
    bytes[2] = static_cast<std::uint8_t>((addrLe >> 16) & 0xffU);
    bytes[3] = static_cast<std::uint8_t>((addrLe >> 24) & 0xffU);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
    return std::string(buf);
  }

  // Tracks in-flight async refresh operations so we only emit state changes after all complete.
  struct PendingRefresh {
    std::vector<AccessPointInfo> capturedAps;
    std::vector<VpnConnectionInfo> capturedVpns;
    std::vector<std::string> capturedSaved;
    int pendingOps = 0;
    std::function<void()> onAllComplete;
  };

  struct SavedConnectionsState {
    std::vector<std::string> ssids;
    int pending = 0;
  };

  struct VpnRefreshState {
    std::vector<VpnConnectionInfo> vpns;
    std::set<std::string> vpnPaths;
    int pending = 0;
  };

  struct ActiveVpnState {
    std::set<std::string> activeProfilePaths;
    int pending = 0;
  };

  struct DeviceAccessPointsState {
    std::vector<AccessPointInfo> aps;
    int pendingDevices = 0;
  };

  struct AccessPointBatchState {
    std::vector<AccessPointInfo> aps;
    int pendingAps = 0;
  };

} // namespace

struct NetworkManagerService::PendingAccessPointActivation {
  std::string ssid;
  std::string connectionPath;
  std::unique_ptr<sdbus::IProxy> activeProxy;
};

NetworkManagerService::NetworkManagerService(SystemBus& bus) : m_bus(bus) {
  if (!bus.nameHasOwner("org.freedesktop.NetworkManager")) {
    throw sdbus::Error(
        sdbus::Error::Name{"org.freedesktop.DBus.Error.ServiceUnknown"},
        "The name org.freedesktop.NetworkManager was not provided by any .service files"
    );
  }
  m_lifetimeToken = std::make_shared<int>(0);
  m_nm = sdbus::createProxy(m_bus.connection(), kNmBusName, kNmObjectPath);

  m_nm->uponSignal("PropertiesChanged")
      .onInterface(kPropertiesInterface)
      .call([this](
                const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& changedProperties,
                const std::vector<std::string>& /*invalidatedProperties*/
            ) {
        if (interfaceName != kNmInterface) {
          return;
        }
        bool wirelessNowOn = false;
        if (auto it = changedProperties.find("WirelessEnabled"); it != changedProperties.end()) {
          try {
            wirelessNowOn = it->second.get<bool>();
          } catch (const sdbus::Error&) {
          }
        }
        if (changedProperties.contains("PrimaryConnection") || changedProperties.contains("ActiveConnections") ||
            changedProperties.contains("WirelessEnabled") || changedProperties.contains("State") ||
            changedProperties.contains("Connectivity")) {
          rebindActiveConnection();
        }
        if (wirelessNowOn) {
          // NM powered the radio on but the wifi device is still transitioning
          // out of Unavailable, so calling RequestScan now would be rejected.
          // NM starts its own scan as soon as the device reaches Disconnected;
          // just mark ourselves scanning and snapshot LastScan so the device
          // PropertiesChanged watcher clears the flag when the scan finishes.
          std::int64_t baseline = 0;
          try {
            std::vector<sdbus::ObjectPath> devices;
            m_nm->callMethod("GetDevices").onInterface(kNmInterface).storeResultsTo(devices);
            for (const auto& devicePath : devices) {
              try {
                auto device = sdbus::createProxy(m_bus.connection(), kNmBusName, devicePath);
                const auto deviceType = getPropertyOr<std::uint32_t>(*device, kNmDeviceInterface, "DeviceType", 0U);
                if (deviceType != kNmDeviceTypeWifi) {
                  continue;
                }
                const auto lastScan =
                    getPropertyOr<std::int64_t>(*device, kNmDeviceWirelessInterface, "LastScan", std::int64_t{0});
                if (lastScan > baseline) {
                  baseline = lastScan;
                }
              } catch (const sdbus::Error&) {
              }
            }
          } catch (const sdbus::Error&) {
          }
          m_scanning = true;
          m_scanBaselineLastScan = baseline;
          refresh();
        }
      });

  rebindActiveConnection();
  requestScan();
}

NetworkManagerService::~NetworkManagerService() { m_lifetimeToken.reset(); }

void NetworkManagerService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void NetworkManagerService::refresh() {
  const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
  if (m_refreshInFlight) {
    m_refreshQueued = true;
    return;
  }
  m_refreshInFlight = true;

  auto pending = std::make_shared<PendingRefresh>();
  pending->capturedAps = m_accessPoints;
  pending->capturedVpns = m_vpnConnections;
  pending->capturedSaved = m_savedSsids;
  pending->pendingOps = 3;

  pending->onAllComplete = [this, pending, lifetimeToken]() {
    if (lifetimeToken.expired()) {
      return;
    }
    readStateAsync([this, pending, lifetimeToken](NetworkState next) {
      if (lifetimeToken.expired()) {
        return;
      }
      const bool apsChanged = pending->capturedAps != m_accessPoints;
      const bool vpnsChanged = pending->capturedVpns != m_vpnConnections;
      const bool savedChanged = pending->capturedSaved != m_savedSsids;
      const bool stateChanged = next != m_state;
      const bool firstSnapshot = !m_hasStateSnapshot;
      const bool wirelessEnabledChanged = next.wirelessEnabled != m_state.wirelessEnabled;
      const NetworkChangeOrigin origin = wirelessEnabledChanged
                                             ? consumeWirelessEnabledChangeOrigin(next.wirelessEnabled)
                                             : NetworkChangeOrigin::External;
      m_state = std::move(next);
      m_hasStateSnapshot = true;
      if ((firstSnapshot || stateChanged || apsChanged || vpnsChanged || savedChanged) && m_changeCallback) {
        m_changeCallback(m_state, origin);
      }
      // Break the self-reference cycle: pending->onAllComplete captures pending.
      pending->onAllComplete = {};

      m_refreshInFlight = false;
      if (m_refreshQueued) {
        m_refreshQueued = false;
        refresh();
      }
    });
  };

  auto onOpComplete = [pending, lifetimeToken]() {
    if (lifetimeToken.expired()) {
      return;
    }
    if (--pending->pendingOps == 0) {
      pending->onAllComplete();
    }
  };

  refreshAccessPoints(onOpComplete);
  refreshVpnConnections(onOpComplete);
  refreshSavedConnections(onOpComplete);
}

void NetworkManagerService::requestScan() {
  std::int64_t baseline = 0;
  bool anyRequested = false;
  try {
    std::vector<sdbus::ObjectPath> devices;
    m_nm->callMethod("GetDevices").onInterface(kNmInterface).storeResultsTo(devices);
    for (const auto& devicePath : devices) {
      try {
        auto device = sdbus::createProxy(m_bus.connection(), kNmBusName, devicePath);
        const auto deviceType = getPropertyOr<std::uint32_t>(*device, kNmDeviceInterface, "DeviceType", 0U);
        if (deviceType != kNmDeviceTypeWifi) {
          continue;
        }
        const auto lastScan =
            getPropertyOr<std::int64_t>(*device, kNmDeviceWirelessInterface, "LastScan", std::int64_t{0});
        if (lastScan > baseline) {
          baseline = lastScan;
        }
        const std::map<std::string, sdbus::Variant> options;
        device->callMethod("RequestScan").onInterface(kNmDeviceWirelessInterface).withArguments(options);
        anyRequested = true;
      } catch (const sdbus::Error& e) {
        kLog.debug("RequestScan failed on {}: {}", std::string(devicePath), e.what());
      }
    }
  } catch (const sdbus::Error& e) {
    kLog.warn("GetDevices failed: {}", e.what());
  }
  if (anyRequested) {
    m_scanning = true;
    m_scanBaselineLastScan = baseline;
    refresh();
  }
}

bool NetworkManagerService::activateAccessPoint(const AccessPointInfo& ap) {
  if (ap.devicePath.empty() || ap.path.empty()) {
    return false;
  }
  if (ap.active) {
    return true;
  }

  // Only try ActivateConnection("/") when we actually have a saved profile for
  // this SSID — NM matches by best fit, and a stray saved connection (e.g. for
  // another device, or a profile we thought was forgotten) would otherwise be
  // silently reused with whatever PSK it carries. When there is no saved
  // profile we create a temporary profile below. Secured new networks must use
  // the psk overload so the current connection is not torn down just to ask for
  // credentials.
  if (hasSavedConnection(ap.ssid)) {
    try {
      const sdbus::ObjectPath emptyConnectionPath{"/"};
      const sdbus::ObjectPath devicePath{ap.devicePath};
      const sdbus::ObjectPath apPath{ap.path};
      sdbus::ObjectPath activePath;
      m_nm->callMethod("ActivateConnection")
          .onInterface(kNmInterface)
          .withArguments(emptyConnectionPath, devicePath, apPath)
          .storeResultsTo(activePath);
      kLog.info("activating ap ssid={} active={}", ap.ssid, std::string(activePath));
      return true;
    } catch (const sdbus::Error& e) {
      kLog.debug("ActivateConnection(/) failed for ssid={}: {}; trying AddAndActivate", ap.ssid, e.what());
    }
  }

  if (ap.secured) {
    return false;
  }
  return addAndActivateAccessPoint(ap, std::nullopt);
}

bool NetworkManagerService::activateAccessPoint(const AccessPointInfo& ap, const std::string& psk) {
  if (ap.devicePath.empty() || ap.path.empty()) {
    return false;
  }
  if (ap.active) {
    return true;
  }
  if (ap.secured && psk.empty()) {
    return false;
  }
  return addAndActivateAccessPoint(ap, psk);
}

bool NetworkManagerService::addAndActivateAccessPoint(
    const AccessPointInfo& ap, const std::optional<std::string>& psk
) {
  try {
    ConnectionSettings settings;
    if (ap.secured) {
      // Minimal secured-wifi settings — NM fills in ssid from the specific_object.
      settings["802-11-wireless-security"]["key-mgmt"] = sdbus::Variant{std::string("wpa-psk")};
      if (psk.has_value()) {
        settings["802-11-wireless-security"]["psk"] = sdbus::Variant{*psk};
      }
    }
    const sdbus::ObjectPath devicePath{ap.devicePath};
    const sdbus::ObjectPath apPath{ap.path};
    sdbus::ObjectPath connectionPath;
    sdbus::ObjectPath activePath;
    VariantMap result;
    const VariantMap options{{"persist", sdbus::Variant{std::string("memory")}}};
    try {
      m_nm->callMethod("AddAndActivateConnection2")
          .onInterface(kNmInterface)
          .withArguments(settings, devicePath, apPath, options)
          .storeResultsTo(connectionPath, activePath, result);
    } catch (const sdbus::Error& e) {
      if (e.getName() != sdbus::Error::Name{"org.freedesktop.DBus.Error.UnknownMethod"}) {
        throw;
      }
      kLog.debug(
          "AddAndActivateConnection2 unavailable for ssid={}; falling back to AddAndActivateConnection", ap.ssid
      );
      m_nm->callMethod("AddAndActivateConnection")
          .onInterface(kNmInterface)
          .withArguments(settings, devicePath, apPath)
          .storeResultsTo(connectionPath, activePath);
    }
    kLog.info(
        "add+activate ap ssid={} conn={} active={}", ap.ssid, std::string(connectionPath), std::string(activePath)
    );
    watchPendingAccessPointActivation(ap.ssid, std::string(connectionPath), std::string(activePath));
    refresh();
    return true;
  } catch (const sdbus::Error& e) {
    kLog.warn("AddAndActivateConnection failed ssid={} err={}", ap.ssid, e.what());
    return false;
  }
}

void NetworkManagerService::watchPendingAccessPointActivation(
    const std::string& ssid, const std::string& connectionPath, const std::string& activePath
) {
  if (activePath.empty() || activePath == "/") {
    return;
  }
  try {
    auto pending = std::make_unique<PendingAccessPointActivation>();
    pending->ssid = ssid;
    pending->connectionPath = connectionPath;
    pending->activeProxy = sdbus::createProxy(m_bus.connection(), kNmBusName, sdbus::ObjectPath{activePath});

    const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
    pending->activeProxy->uponSignal("PropertiesChanged")
        .onInterface(kPropertiesInterface)
        .call([this, lifetimeToken, activePath](
                  const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& changedProperties,
                  const std::vector<std::string>& /*invalidatedProperties*/
              ) {
          if (lifetimeToken.expired() || interfaceName != kNmActiveConnectionInterface) {
            return;
          }
          auto stateIt = changedProperties.find("State");
          if (stateIt == changedProperties.end()) {
            return;
          }
          try {
            handlePendingAccessPointActivationState(activePath, stateIt->second.get<std::uint32_t>());
          } catch (const sdbus::Error&) {
          }
        });

    auto* activeProxy = pending->activeProxy.get();
    m_pendingApActivations[activePath] = std::move(pending);
    const auto state =
        getPropertyOr<std::uint32_t>(*activeProxy, kNmActiveConnectionInterface, "State", std::uint32_t{0});
    handlePendingAccessPointActivationState(activePath, state);
  } catch (const sdbus::Error& e) {
    kLog.debug("pending ap activation watch failed ssid={} active={}: {}", ssid, activePath, e.what());
  }
}

void NetworkManagerService::handlePendingAccessPointActivationState(
    const std::string& activePath, std::uint32_t state
) {
  auto it = m_pendingApActivations.find(activePath);
  if (it == m_pendingApActivations.end()) {
    return;
  }
  if (state == kNmActiveConnectionStateActivated) {
    const std::string ssid = it->second->ssid;
    const std::string connectionPath = it->second->connectionPath;
    m_pendingApActivations.erase(it);
    kLog.info("ap activation succeeded ssid={} conn={}", ssid, connectionPath);
    persistConnectionToDisk(connectionPath, ssid);
    refresh();
    return;
  }
  if (state == kNmActiveConnectionStateDeactivated) {
    const std::string ssid = it->second->ssid;
    const std::string connectionPath = it->second->connectionPath;
    m_pendingApActivations.erase(it);
    kLog.info("ap activation did not complete ssid={} conn={}", ssid, connectionPath);
    deleteUnsavedConnection(connectionPath, ssid);
    refresh();
  }
}

void NetworkManagerService::persistConnectionToDisk(const std::string& connectionPath, const std::string& ssid) {
  if (connectionPath.empty() || connectionPath == "/") {
    return;
  }
  try {
    auto connection = std::shared_ptr<sdbus::IProxy>(
        sdbus::createProxy(m_bus.connection(), kNmBusName, sdbus::ObjectPath{connectionPath})
    );
    const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
    const ConnectionSettings settings;
    const VariantMap args;
    connection->callMethodAsync("Update2")
        .onInterface(kNmSettingsConnectionInterface)
        .withArguments(settings, k_nmSettingsUpdate2FlagToDisk, args)
        .uponReplyInvoke([this, lifetimeToken, connection, connectionPath,
                          ssid](std::optional<sdbus::Error> err, VariantMap /*result*/) {
          if (lifetimeToken.expired()) {
            return;
          }
          if (err.has_value()) {
            kLog.warn("persist connection failed ssid={} conn={}: {}", ssid, connectionPath, err->what());
          } else {
            kLog.info("persisted connection ssid={} conn={}", ssid, connectionPath);
          }
          refresh();
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("persist connection dispatch failed ssid={} conn={}: {}", ssid, connectionPath, e.what());
  }
}

void NetworkManagerService::deleteUnsavedConnection(const std::string& connectionPath, const std::string& ssid) {
  if (connectionPath.empty() || connectionPath == "/") {
    return;
  }
  try {
    auto connection = std::shared_ptr<sdbus::IProxy>(
        sdbus::createProxy(m_bus.connection(), kNmBusName, sdbus::ObjectPath{connectionPath})
    );
    const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
    connection->callMethodAsync("Delete")
        .onInterface(kNmSettingsConnectionInterface)
        .uponReplyInvoke([this, lifetimeToken, connection, connectionPath, ssid](std::optional<sdbus::Error> err) {
          if (lifetimeToken.expired()) {
            return;
          }
          if (err.has_value()) {
            kLog.warn("delete unsaved connection failed ssid={} conn={}: {}", ssid, connectionPath, err->what());
          } else {
            kLog.info("deleted unsaved connection ssid={} conn={}", ssid, connectionPath);
          }
          refresh();
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("delete unsaved connection dispatch failed ssid={} conn={}: {}", ssid, connectionPath, e.what());
  }
}

bool NetworkManagerService::activateVpnConnection(const VpnConnectionInfo& vpn) {
  if (vpn.path.empty()) {
    return false;
  }
  try {
    // Async: ActivateConnection can involve polkit/agent interactions, and a
    // synchronous call can stall the main loop while authorization is pending.
    const std::string vpnName = vpn.name;
    const std::string vpnPath = vpn.path;
    const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
    m_nm->callMethodAsync("ActivateConnection")
        .onInterface(kNmInterface)
        .withArguments(sdbus::ObjectPath{vpnPath}, sdbus::ObjectPath{"/"}, sdbus::ObjectPath{"/"})
        .uponReplyInvoke([this, lifetimeToken, vpnName,
                          vpnPath](std::optional<sdbus::Error> err, sdbus::ObjectPath activePath) {
          if (lifetimeToken.expired()) {
            return;
          }
          if (err.has_value()) {
            kLog.warn("ActivateConnection(vpn) failed name={} path={}: {}", vpnName, vpnPath, err->what());
          } else {
            kLog.info("activating vpn name={} active={}", vpnName, std::string(activePath));
          }
          refresh();
        });
    return true;
  } catch (const sdbus::Error& e) {
    kLog.warn("ActivateConnection(vpn) failed name={} path={} err={}", vpn.name, vpn.path, e.what());
    return false;
  }
}

bool NetworkManagerService::deactivateVpnConnection(const VpnConnectionInfo& vpn) {
  if (vpn.path.empty()) {
    return false;
  }
  try {
    std::vector<sdbus::ObjectPath> activeConnections;
    const sdbus::Variant activeVar = m_nm->getProperty("ActiveConnections").onInterface(kNmInterface);
    activeConnections = activeVar.get<std::vector<sdbus::ObjectPath>>();
    for (const auto& activePath : activeConnections) {
      try {
        auto active = sdbus::createProxy(m_bus.connection(), kNmBusName, activePath);
        const auto profilePath =
            getPropertyOr<sdbus::ObjectPath>(*active, kNmActiveConnectionInterface, "Connection", sdbus::ObjectPath{});
        const auto activeState = getPropertyOr<std::uint32_t>(*active, kNmActiveConnectionInterface, "State", 0U);
        if (profilePath != vpn.path || activeState != kNmActiveConnectionStateActivated) {
          continue;
        }
        // Async: DeactivateConnection on a system-owned profile is gated by polkit,
        // and a sync call would freeze the main loop while the polkit agent prompts
        // (or while polkit waits for an agent to register). Fire-and-forget here.
        const std::string activePathStr = std::string(activePath);
        const std::string vpnName = vpn.name;
        m_nm->callMethodAsync("DeactivateConnection")
            .onInterface(kNmInterface)
            .withArguments(sdbus::ObjectPath{activePathStr})
            .uponReplyInvoke([activePathStr, vpnName](std::optional<sdbus::Error> err) {
              if (err.has_value()) {
                kLog.warn(
                    "DeactivateConnection(vpn) failed name={} active={}: {}", vpnName, activePathStr, err->what()
                );
              } else {
                kLog.info("deactivated vpn name={} active={}", vpnName, activePathStr);
              }
            });
        return true;
      } catch (const sdbus::Error&) {
      }
    }
  } catch (const sdbus::Error& e) {
    kLog.warn("DeactivateConnection(vpn) lookup failed path={}: {}", vpn.path, e.what());
    return false;
  }
  return false;
}

void NetworkManagerService::setWirelessEnabled(bool enabled) {
  if (enabled) {
    const RfkillSwitchResult rfkillResult = setRfkillSoftBlocked(RfkillDeviceType::Wlan, false);
    if (rfkillResult.hardBlocked) {
      kLog.warn("setWirelessEnabled: wlan rfkill hard block is active");
      return;
    }
    if (!rfkillResult.success) {
      kLog.warn("setWirelessEnabled: rfkill unblock failed ({}), trying NetworkManager anyway", rfkillResult.detail);
    }
  }
  if (enabled != m_state.wirelessEnabled) {
    m_pendingLocalWirelessEnabled = enabled;
  }
  try {
    m_nm->setProperty("WirelessEnabled").onInterface(kNmInterface).toValue(enabled);
  } catch (const sdbus::Error& e) {
    if (m_pendingLocalWirelessEnabled == enabled) {
      m_pendingLocalWirelessEnabled.reset();
    }
    kLog.warn("WirelessEnabled write failed: {}", e.what());
  }
}

void NetworkManagerService::disconnect() {
  if (m_activeConnectionPath.empty() || m_activeConnectionPath == "/") {
    return;
  }
  // Async: DeactivateConnection on a system-owned profile is gated by polkit,
  // and a sync call would freeze the main loop while the polkit agent prompts
  // (or while polkit waits for an agent to register). Fire-and-forget here.
  const std::string activePath = m_activeConnectionPath;
  try {
    m_nm->callMethodAsync("DeactivateConnection")
        .onInterface(kNmInterface)
        .withArguments(sdbus::ObjectPath{activePath})
        .uponReplyInvoke([activePath](std::optional<sdbus::Error> err) {
          if (err.has_value()) {
            kLog.warn("DeactivateConnection failed path={}: {}", activePath, err->what());
          } else {
            kLog.info("deactivated connection path={}", activePath);
          }
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("DeactivateConnection dispatch failed: {}", e.what());
  }
}

namespace {
  // State machine for an in-flight forgetSsid operation. Owned by lambdas
  // captured via shared_ptr; lives until the last D-Bus reply lands.
  struct ForgetOp {
    std::string ssid;
    std::unique_ptr<sdbus::IProxy> settings;
    std::vector<std::unique_ptr<sdbus::IProxy>> targets;
    int matched = 0;
    int removed = 0;
    int failed = 0;
    int pendingGetSettings = 0;
    int pendingDeletes = 0;
    bool listingDone = false;
    std::function<void()> onComplete;
  };

  bool ssidFromSettings(const std::map<std::string, std::map<std::string, sdbus::Variant>>& cfg, std::string& out) {
    auto wifiIt = cfg.find("802-11-wireless");
    if (wifiIt == cfg.end())
      return false;
    auto ssidIt = wifiIt->second.find("ssid");
    if (ssidIt == wifiIt->second.end())
      return false;
    try {
      const auto bytes = ssidIt->second.get<std::vector<std::uint8_t>>();
      out.assign(bytes.begin(), bytes.end());
      return true;
    } catch (const sdbus::Error&) {
      return false;
    }
  }

  void maybeFinishForget(const std::shared_ptr<ForgetOp>& op) {
    if (op->listingDone && op->pendingGetSettings == 0 && op->pendingDeletes == 0) {
      kLog.info(
          "forgetSsid ssid=\"{}\" matched={} removed={} failed={}", op->ssid, op->matched, op->removed, op->failed
      );
      if (op->onComplete)
        op->onComplete();
    }
  }
} // namespace

void NetworkManagerService::forgetSsid(const std::string& ssid) {
  if (ssid.empty()) {
    return;
  }
  // Tear down the live connection before deleting the saved profile, so a
  // subsequent reconnect attempt cannot silently reuse the still-active
  // connection (which would skip the password prompt). Async — see disconnect().
  if (m_state.kind == NetworkConnectivity::Wireless && m_state.connected && m_state.ssid == ssid) {
    disconnect();
  }

  auto op = std::make_shared<ForgetOp>();
  op->ssid = ssid;
  const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
  op->onComplete = [this, lifetimeToken]() {
    if (lifetimeToken.expired()) {
      return;
    }
    // Final refresh rebuilds the UI (no Forget button, no active tint) without
    // waiting for an NM PropertiesChanged signal to land.
    refresh();
  };

  try {
    op->settings = sdbus::createProxy(m_bus.connection(), kNmBusName, kNmSettingsObjectPath);
  } catch (const sdbus::Error& e) {
    kLog.warn("forgetSsid: settings proxy failed ssid=\"{}\": {}", ssid, e.what());
    refresh();
    return;
  }

  auto& bus = m_bus;
  op->settings->callMethodAsync("ListConnections")
      .onInterface(kNmSettingsInterface)
      .uponReplyInvoke([op, &bus](std::optional<sdbus::Error> err, std::vector<sdbus::ObjectPath> paths) {
        if (err.has_value()) {
          kLog.warn("forgetSsid: ListConnections failed ssid=\"{}\": {}", op->ssid, err->what());
          op->listingDone = true;
          maybeFinishForget(op);
          return;
        }
        for (const auto& connectionPath : paths) {
          std::unique_ptr<sdbus::IProxy> conn;
          try {
            conn = sdbus::createProxy(bus.connection(), kNmBusName, connectionPath);
          } catch (const sdbus::Error& e) {
            kLog.debug("forgetSsid: proxy failed for {}: {}", std::string(connectionPath), e.what());
            continue;
          }
          auto* connRaw = conn.get();
          op->targets.push_back(std::move(conn));
          ++op->pendingGetSettings;
          const std::string pathStr{connectionPath};
          connRaw->callMethodAsync("GetSettings")
              .onInterface(kNmSettingsConnectionInterface)
              .uponReplyInvoke([op, connRaw, pathStr](
                                   std::optional<sdbus::Error> getErr,
                                   std::map<std::string, std::map<std::string, sdbus::Variant>> cfg
                               ) {
                --op->pendingGetSettings;
                if (getErr.has_value()) {
                  kLog.debug("forgetSsid: GetSettings failed for {}: {}", pathStr, getErr->what());
                  maybeFinishForget(op);
                  return;
                }
                std::string foundSsid;
                if (!ssidFromSettings(cfg, foundSsid) || foundSsid != op->ssid) {
                  maybeFinishForget(op);
                  return;
                }
                ++op->matched;
                ++op->pendingDeletes;
                connRaw->callMethodAsync("Delete")
                    .onInterface(kNmSettingsConnectionInterface)
                    .uponReplyInvoke([op, pathStr](std::optional<sdbus::Error> delErr) {
                      --op->pendingDeletes;
                      if (delErr.has_value()) {
                        // Common cause: system-owned profile + no polkit agent
                        // running, so Delete is denied. Surface the real error
                        // name — otherwise indistinguishable from "nothing happened".
                        ++op->failed;
                        kLog.warn(
                            "forgetSsid: Delete refused for {} ssid=\"{}\": {}", pathStr, op->ssid, delErr->what()
                        );
                      } else {
                        ++op->removed;
                      }
                      maybeFinishForget(op);
                    });
                maybeFinishForget(op);
              });
        }
        op->listingDone = true;
        maybeFinishForget(op);
      });
}

bool NetworkManagerService::hasSavedConnection(const std::string& ssid) const {
  if (ssid.empty()) {
    return false;
  }
  for (const auto& [activePath, pending] : m_pendingApActivations) {
    (void)activePath;
    if (pending != nullptr && pending->ssid == ssid) {
      return false;
    }
  }
  return std::find(m_savedSsids.begin(), m_savedSsids.end(), ssid) != m_savedSsids.end();
}

void NetworkManagerService::refreshSavedConnections(std::function<void()> onComplete) {
  const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
  try {
    auto settings =
        std::shared_ptr<sdbus::IProxy>(sdbus::createProxy(m_bus.connection(), kNmBusName, kNmSettingsObjectPath));
    settings->callMethodAsync("ListConnections")
        .onInterface(kNmSettingsInterface)
        .uponReplyInvoke([this, lifetimeToken, settings,
                          onComplete](std::optional<sdbus::Error> err, std::vector<sdbus::ObjectPath> connectionPaths) {
          if (lifetimeToken.expired()) {
            return;
          }
          if (err.has_value()) {
            kLog.debug("refreshSavedConnections ListConnections failed: {}", err->what());
            onComplete();
            return;
          }

          if (connectionPaths.empty()) {
            m_savedSsids.clear();
            onComplete();
            return;
          }

          auto savedState = std::make_shared<SavedConnectionsState>();
          savedState->pending = static_cast<int>(connectionPaths.size());

          for (const auto& connectionPath : connectionPaths) {
            try {
              auto connection =
                  std::shared_ptr<sdbus::IProxy>(sdbus::createProxy(m_bus.connection(), kNmBusName, connectionPath));
              const auto flags =
                  getPropertyOr<std::uint32_t>(*connection, kNmSettingsConnectionInterface, "Flags", std::uint32_t{0});
              const auto filename =
                  getPropertyOr<std::string>(*connection, kNmSettingsConnectionInterface, "Filename", {});
              if ((flags & kNmSettingsConnectionFlagUnsaved) != 0U && filename.empty()) {
                if (--savedState->pending == 0) {
                  finishSavedConnections(savedState->ssids, onComplete);
                }
                continue;
              }
              connection->callMethodAsync("GetSettings")
                  .onInterface(kNmSettingsConnectionInterface)
                  .uponReplyInvoke([this, lifetimeToken, connection, savedState, onComplete](
                                       std::optional<sdbus::Error> settingsErr,
                                       std::map<std::string, std::map<std::string, sdbus::Variant>> cfg
                                   ) {
                    if (lifetimeToken.expired()) {
                      return;
                    }
                    if (!settingsErr.has_value()) {
                      auto wifiIt = cfg.find("802-11-wireless");
                      if (wifiIt != cfg.end()) {
                        auto ssidIt = wifiIt->second.find("ssid");
                        if (ssidIt != wifiIt->second.end()) {
                          try {
                            const auto bytes = ssidIt->second.get<std::vector<std::uint8_t>>();
                            std::string ssid(bytes.begin(), bytes.end());
                            if (!ssid.empty()) {
                              savedState->ssids.push_back(std::move(ssid));
                            }
                          } catch (const sdbus::Error&) {
                          }
                        }
                      }
                    }
                    if (--savedState->pending == 0) {
                      finishSavedConnections(savedState->ssids, onComplete);
                    }
                  });
            } catch (const sdbus::Error&) {
              if (--savedState->pending == 0) {
                finishSavedConnections(savedState->ssids, onComplete);
              }
            }
          }
        });
  } catch (const sdbus::Error& e) {
    kLog.debug("refreshSavedConnections: {}", e.what());
    onComplete();
  }
}

void NetworkManagerService::refreshVpnConnections(std::function<void()> onComplete) {
  const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
  try {
    auto settings =
        std::shared_ptr<sdbus::IProxy>(sdbus::createProxy(m_bus.connection(), kNmBusName, kNmSettingsObjectPath));
    settings->callMethodAsync("ListConnections")
        .onInterface(kNmSettingsInterface)
        .uponReplyInvoke([this, lifetimeToken, settings,
                          onComplete](std::optional<sdbus::Error> err, std::vector<sdbus::ObjectPath> connectionPaths) {
          if (lifetimeToken.expired()) {
            return;
          }
          if (err.has_value()) {
            kLog.debug("refreshVpnConnections ListConnections failed: {}", err->what());
            onComplete();
            return;
          }

          if (connectionPaths.empty()) {
            m_vpnConnections.clear();
            onComplete();
            return;
          }

          auto vpnState = std::make_shared<VpnRefreshState>();
          vpnState->pending = static_cast<int>(connectionPaths.size());

          auto finalize = [this, lifetimeToken, vpnState, onComplete]() {
            if (lifetimeToken.expired()) {
              return;
            }
            std::ranges::sort(vpnState->vpns, [](const VpnConnectionInfo& a, const VpnConnectionInfo& b) {
              if (a.active != b.active) {
                return a.active;
              }
              return a.name < b.name;
            });
            m_vpnConnections = std::move(vpnState->vpns);
            onComplete();
          };

          auto markActiveAndFinalize = [this, lifetimeToken, vpnState, finalize]() {
            if (lifetimeToken.expired()) {
              return;
            }
            m_nm->callMethodAsync("Get")
                .onInterface(kPropertiesInterface)
                .withArguments(kNmInterface, "ActiveConnections")
                .uponReplyInvoke([this, lifetimeToken, vpnState,
                                  finalize](std::optional<sdbus::Error> activeListErr, sdbus::Variant activeListValue) {
                  if (lifetimeToken.expired()) {
                    return;
                  }
                  if (activeListErr.has_value()) {
                    kLog.debug("refreshVpnConnections active list failed: {}", activeListErr->what());
                    finalize();
                    return;
                  }

                  std::vector<sdbus::ObjectPath> activePaths;
                  try {
                    activePaths = activeListValue.get<std::vector<sdbus::ObjectPath>>();
                  } catch (const sdbus::Error&) {
                    finalize();
                    return;
                  }

                  if (activePaths.empty()) {
                    finalize();
                    return;
                  }

                  auto activeState = std::make_shared<ActiveVpnState>();
                  activeState->pending = static_cast<int>(activePaths.size());

                  auto onActiveComplete = [lifetimeToken, vpnState, activeState, finalize]() {
                    if (lifetimeToken.expired()) {
                      return;
                    }
                    if (--activeState->pending == 0) {
                      for (auto& vpn : vpnState->vpns) {
                        if (activeState->activeProfilePaths.contains(vpn.path)) {
                          vpn.active = true;
                        }
                      }
                      finalize();
                    }
                  };

                  for (const auto& activePath : activePaths) {
                    try {
                      auto active = std::shared_ptr<sdbus::IProxy>(
                          sdbus::createProxy(m_bus.connection(), kNmBusName, activePath)
                      );
                      active->callMethodAsync("GetAll")
                          .onInterface(kPropertiesInterface)
                          .withArguments(kNmActiveConnectionInterface)
                          .uponReplyInvoke([lifetimeToken, active, activeState, onActiveComplete](
                                               std::optional<sdbus::Error> getAllErr,
                                               std::map<std::string, sdbus::Variant> properties
                                           ) {
                            if (lifetimeToken.expired()) {
                              return;
                            }
                            if (!getAllErr.has_value()) {
                              std::uint32_t state = 0U;
                              if (auto stateIt = properties.find("State"); stateIt != properties.end()) {
                                try {
                                  state = stateIt->second.get<std::uint32_t>();
                                } catch (const sdbus::Error&) {
                                  state = 0U;
                                }
                              }

                              if (state == kNmActiveConnectionStateActivating ||
                                  state == kNmActiveConnectionStateActivated) {
                                if (auto connIt = properties.find("Connection"); connIt != properties.end()) {
                                  try {
                                    const auto profilePath = connIt->second.get<sdbus::ObjectPath>();
                                    activeState->activeProfilePaths.insert(std::string(profilePath));
                                  } catch (const sdbus::Error&) {
                                  }
                                }
                              }
                            }
                            onActiveComplete();
                          });
                    } catch (const sdbus::Error&) {
                      onActiveComplete();
                    }
                  }
                });
          };

          for (const auto& connectionPath : connectionPaths) {
            try {
              auto connection =
                  std::shared_ptr<sdbus::IProxy>(sdbus::createProxy(m_bus.connection(), kNmBusName, connectionPath));
              connection->callMethodAsync("GetSettings")
                  .onInterface(kNmSettingsConnectionInterface)
                  .uponReplyInvoke([lifetimeToken, connection, vpnState, connectionPath, markActiveAndFinalize,
                                    onComplete](
                                       std::optional<sdbus::Error> getErr,
                                       std::map<std::string, std::map<std::string, sdbus::Variant>> cfg
                                   ) {
                    if (lifetimeToken.expired()) {
                      return;
                    }
                    if (!getErr.has_value()) {
                      auto connIt = cfg.find("connection");
                      if (connIt != cfg.end()) {
                        auto typeIt = connIt->second.find("type");
                        if (typeIt != connIt->second.end()) {
                          try {
                            const auto type = typeIt->second.get<std::string>();
                            const bool hasVpnSection = cfg.contains("vpn");
                            const bool vpnLikeType = type == "vpn" || type == "wireguard";
                            if (vpnLikeType || hasVpnSection) {
                              VpnConnectionInfo info;
                              info.path = std::string(connectionPath);
                              auto idIt = connIt->second.find("id");
                              if (idIt != connIt->second.end()) {
                                try {
                                  info.name = idIt->second.get<std::string>();
                                } catch (const sdbus::Error&) {
                                }
                              }
                              if (info.name.empty()) {
                                info.name = info.path;
                              }
                              info.active = false;
                              vpnState->vpnPaths.insert(info.path);
                              vpnState->vpns.push_back(std::move(info));
                            }
                          } catch (const sdbus::Error&) {
                          }
                        }
                      }
                    }
                    if (--vpnState->pending == 0) {
                      markActiveAndFinalize();
                    }
                  });
            } catch (const sdbus::Error&) {
              if (--vpnState->pending == 0) {
                markActiveAndFinalize();
              }
            }
          }
        });
  } catch (const sdbus::Error& e) {
    kLog.debug("refreshVpnConnections: {}", e.what());
    onComplete();
  }
}

void NetworkManagerService::ensureWifiDeviceSubscribed(const std::string& devicePath) {
  if (m_wifiDevices.contains(devicePath)) {
    return;
  }
  try {
    auto proxy = sdbus::createProxy(m_bus.connection(), kNmBusName, sdbus::ObjectPath{devicePath});
    proxy->uponSignal("PropertiesChanged")
        .onInterface(kPropertiesInterface)
        .call([this](
                  const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& changedProperties,
                  const std::vector<std::string>& /*invalidatedProperties*/
              ) {
          if (interfaceName == kNmDeviceWirelessInterface) {
            if (auto it = changedProperties.find("LastScan"); it != changedProperties.end()) {
              try {
                const auto lastScan = it->second.get<std::int64_t>();
                if (m_scanning && lastScan > m_scanBaselineLastScan) {
                  m_scanning = false;
                }
              } catch (const sdbus::Error&) {
              }
            }
            if (changedProperties.contains("AccessPoints") || changedProperties.contains("LastScan")) {
              refresh();
            }
          } else if (interfaceName == kNmDeviceInterface) {
            if (changedProperties.contains("State")) {
              refresh();
            }
          }
        });
    m_wifiDevices.emplace(devicePath, std::move(proxy));
  } catch (const sdbus::Error& e) {
    kLog.debug("wifi device subscribe failed {}: {}", devicePath, e.what());
  }
}

void NetworkManagerService::refreshAccessPoints(std::function<void()> onComplete) {
  const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
  try {
    m_nm->callMethodAsync("GetDevices")
        .onInterface(kNmInterface)
        .uponReplyInvoke([this, lifetimeToken,
                          onComplete](std::optional<sdbus::Error> err, std::vector<sdbus::ObjectPath> devices) {
          if (lifetimeToken.expired()) {
            return;
          }
          if (err.has_value()) {
            kLog.debug("refreshAccessPoints GetDevices failed: {}", err->what());
            onComplete();
            return;
          }

          if (devices.empty()) {
            m_accessPoints.clear();
            onComplete();
            return;
          }

          // One slot per device; non-WiFi devices decrement immediately without contributing APs.
          const int totalDevices = static_cast<int>(devices.size());
          auto deviceState = std::make_shared<DeviceAccessPointsState>();
          deviceState->pendingDevices = totalDevices;

          for (const auto& devicePath : devices) {
            try {
              auto device =
                  std::shared_ptr<sdbus::IProxy>(sdbus::createProxy(m_bus.connection(), kNmBusName, devicePath));
              // GetAll on DBus.Properties with the wireless interface arg: succeeds only for
              // WiFi devices and also gives us ActiveAccessPoint — no sync reads needed.
              device->callMethodAsync("GetAll")
                  .onInterface(kPropertiesInterface)
                  .withArguments(kNmDeviceWirelessInterface)
                  .uponReplyInvoke([this, lifetimeToken, device, deviceState, devicePath, onComplete](
                                       std::optional<sdbus::Error> wifiErr,
                                       std::map<std::string, sdbus::Variant> wifiProps
                                   ) {
                    if (lifetimeToken.expired()) {
                      return;
                    }
                    if (wifiErr.has_value()) {
                      // Not a WiFi device — just decrement and possibly finish.
                      if (--deviceState->pendingDevices == 0) {
                        finishRefreshAccessPoints(deviceState->aps, onComplete);
                      }
                      return;
                    }

                    // WiFi device confirmed. Subscribe for scan/state signals.
                    ensureWifiDeviceSubscribed(devicePath);

                    std::string activeApPath;
                    if (auto it = wifiProps.find("ActiveAccessPoint"); it != wifiProps.end()) {
                      try {
                        activeApPath = it->second.get<sdbus::ObjectPath>();
                      } catch (const sdbus::Error&) {
                      }
                    }

                    device->callMethodAsync("GetAccessPoints")
                        .onInterface(kNmDeviceWirelessInterface)
                        .uponReplyInvoke(
                            [this, lifetimeToken, device, deviceState, devicePath, activeApPath,
                             onComplete](std::optional<sdbus::Error> apErr, std::vector<sdbus::ObjectPath> apPaths) {
                              if (lifetimeToken.expired()) {
                                return;
                              }
                              if (apErr.has_value() || apPaths.empty()) {
                                if (--deviceState->pendingDevices == 0) {
                                  finishRefreshAccessPoints(deviceState->aps, onComplete);
                                }
                                return;
                              }

                              const int pendingAps = static_cast<int>(apPaths.size());
                              auto apState = std::make_shared<AccessPointBatchState>();
                              apState->pendingAps = pendingAps;

                              for (const auto& apPath : apPaths) {
                                try {
                                  auto ap = std::shared_ptr<sdbus::IProxy>(
                                      sdbus::createProxy(m_bus.connection(), kNmBusName, apPath)
                                  );
                                  ap->callMethodAsync("GetAll")
                                      .onInterface(kPropertiesInterface)
                                      .withArguments(kNmAccessPointInterface)
                                      .uponReplyInvoke([this, lifetimeToken, ap, deviceState, apState, devicePath,
                                                        activeApPath, apPath, onComplete](
                                                           std::optional<sdbus::Error> propErr,
                                                           std::map<std::string, sdbus::Variant> properties
                                                       ) {
                                        if (lifetimeToken.expired()) {
                                          return;
                                        }
                                        if (!propErr.has_value()) {
                                          AccessPointInfo info;
                                          info.path = apPath;
                                          info.devicePath = devicePath;
                                          info.active = !activeApPath.empty() && apPath == activeApPath;
                                          if (auto ssidIt = properties.find("Ssid"); ssidIt != properties.end()) {
                                            try {
                                              const auto ssidBytes = ssidIt->second.get<std::vector<std::uint8_t>>();
                                              info.ssid.assign(ssidBytes.begin(), ssidBytes.end());
                                            } catch (const sdbus::Error&) {
                                            }
                                          }
                                          if (auto strengthIt = properties.find("Strength");
                                              strengthIt != properties.end()) {
                                            try {
                                              info.strength = strengthIt->second.get<std::uint8_t>();
                                            } catch (const sdbus::Error&) {
                                            }
                                          }
                                          const auto wpaFlags = [&properties]() {
                                            if (auto wpaFlagsIt = properties.find("WpaFlags");
                                                wpaFlagsIt != properties.end()) {
                                              try {
                                                return wpaFlagsIt->second.get<std::uint32_t>();
                                              } catch (const sdbus::Error&) {
                                                return 0U;
                                              }
                                            }
                                            return 0U;
                                          }();
                                          const auto rsnFlags = [&properties]() {
                                            if (auto rsnFlagsIt = properties.find("RsnFlags");
                                                rsnFlagsIt != properties.end()) {
                                              try {
                                                return rsnFlagsIt->second.get<std::uint32_t>();
                                              } catch (const sdbus::Error&) {
                                                return 0U;
                                              }
                                            }
                                            return 0U;
                                          }();
                                          info.secured =
                                              (wpaFlags != k_nm80211ApSecNone) || (rsnFlags != k_nm80211ApSecNone);
                                          if (!info.ssid.empty()) {
                                            apState->aps.push_back(std::move(info));
                                          }
                                        }
                                        if (--apState->pendingAps == 0) {
                                          for (auto& apInfo : apState->aps) {
                                            deviceState->aps.push_back(std::move(apInfo));
                                          }
                                          if (--deviceState->pendingDevices == 0) {
                                            finishRefreshAccessPoints(deviceState->aps, onComplete);
                                          }
                                        }
                                      });
                                } catch (const sdbus::Error&) {
                                  if (--apState->pendingAps == 0) {
                                    for (auto& apInfo : apState->aps) {
                                      deviceState->aps.push_back(std::move(apInfo));
                                    }
                                    if (--deviceState->pendingDevices == 0) {
                                      finishRefreshAccessPoints(deviceState->aps, onComplete);
                                    }
                                  }
                                }
                              }
                            }
                        );
                  });
            } catch (const sdbus::Error&) {
              if (--deviceState->pendingDevices == 0) {
                finishRefreshAccessPoints(deviceState->aps, onComplete);
              }
            }
          }
        });
  } catch (const sdbus::Error& e) {
    kLog.debug("refreshAccessPoints: {}", e.what());
    onComplete();
  }
}

void NetworkManagerService::finishSavedConnections(std::vector<std::string>& ssids, std::function<void()> onComplete) {
  std::ranges::sort(ssids);
  ssids.erase(std::unique(ssids.begin(), ssids.end()), ssids.end());
  m_savedSsids = std::move(ssids);
  onComplete();
}

void NetworkManagerService::finishRefreshAccessPoints(
    std::vector<AccessPointInfo>& aps, std::function<void()> onComplete
) {
  // Deduplicate by SSID, keeping the strongest (and marking active if any entry is active).
  std::vector<AccessPointInfo> deduped;
  deduped.reserve(aps.size());
  for (auto& ap : aps) {
    auto it = std::find_if(deduped.begin(), deduped.end(), [&](const AccessPointInfo& other) {
      return other.ssid == ap.ssid;
    });
    if (it == deduped.end()) {
      deduped.push_back(std::move(ap));
      continue;
    }
    if (ap.active) {
      if (!it->active || ap.strength > it->strength) {
        *it = std::move(ap);
      } else {
        it->active = true;
      }
      continue;
    }
    if (it->active) {
      continue;
    }
    if (ap.strength > it->strength) {
      it->strength = ap.strength;
      it->path = ap.path;
      it->devicePath = ap.devicePath;
      it->secured = ap.secured;
    }
  }
  std::ranges::sort(deduped, [](const AccessPointInfo& a, const AccessPointInfo& b) {
    if (a.active != b.active) {
      return a.active;
    }
    return a.strength > b.strength;
  });

  m_accessPoints = std::move(deduped);
  onComplete();
}

void NetworkManagerService::rebindActiveConnection() {
  std::string newPath;
  try {
    const sdbus::Variant value = m_nm->getProperty("PrimaryConnection").onInterface(kNmInterface);
    newPath = value.get<sdbus::ObjectPath>();
  } catch (const sdbus::Error& e) {
    kLog.debug("PrimaryConnection unavailable: {}", e.what());
  }

  if (newPath != m_activeConnectionPath) {
    m_activeConnectionPath = newPath;
    m_activeConnection.reset();
    if (!newPath.empty() && newPath != "/") {
      try {
        m_activeConnection = sdbus::createProxy(m_bus.connection(), kNmBusName, sdbus::ObjectPath{newPath});
        m_activeConnection->uponSignal("PropertiesChanged")
            .onInterface(kPropertiesInterface)
            .call([this](
                      const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& changedProperties,
                      const std::vector<std::string>& /*invalidatedProperties*/
                  ) {
              if (interfaceName != kNmActiveConnectionInterface) {
                return;
              }
              if (changedProperties.contains("Devices") || changedProperties.contains("State") ||
                  changedProperties.contains("Ip4Config")) {
                rebindActiveConnection();
              }
            });
      } catch (const sdbus::Error& e) {
        kLog.debug("active connection proxy failed: {}", e.what());
        m_activeConnection.reset();
      }
    }
  }

  std::string newDevicePath;
  if (m_activeConnection != nullptr) {
    try {
      const sdbus::Variant value = m_activeConnection->getProperty("Devices").onInterface(kNmActiveConnectionInterface);
      const auto devices = value.get<std::vector<sdbus::ObjectPath>>();
      if (!devices.empty()) {
        newDevicePath = devices.front();
      }
    } catch (const sdbus::Error&) {
    }
  }
  rebindActiveDevice(newDevicePath);

  refresh();
}

void NetworkManagerService::rebindActiveDevice(const std::string& devicePath) {
  if (devicePath == m_activeDevicePath && m_activeDevice != nullptr) {
    return;
  }
  m_activeDevicePath = devicePath;
  m_activeDevice.reset();
  rebindActiveAccessPoint({});

  if (devicePath.empty() || devicePath == "/") {
    return;
  }

  try {
    m_activeDevice = sdbus::createProxy(m_bus.connection(), kNmBusName, sdbus::ObjectPath{devicePath});
    m_activeDevice->uponSignal("PropertiesChanged")
        .onInterface(kPropertiesInterface)
        .call([this](
                  const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& changedProperties,
                  const std::vector<std::string>& /*invalidatedProperties*/
              ) {
          if (interfaceName == kNmDeviceInterface) {
            if (changedProperties.contains("Ip4Config") || changedProperties.contains("State") ||
                changedProperties.contains("Interface")) {
              refresh();
            }
          } else if (interfaceName == kNmDeviceWirelessInterface) {
            if (changedProperties.contains("ActiveAccessPoint")) {
              std::string apPath;
              try {
                apPath = changedProperties.at("ActiveAccessPoint").get<sdbus::ObjectPath>();
              } catch (const sdbus::Error&) {
              }
              rebindActiveAccessPoint(apPath);
              refresh();
            }
          }
        });
  } catch (const sdbus::Error& e) {
    kLog.debug("device proxy failed: {}", e.what());
    m_activeDevice.reset();
    return;
  }

  // If this is a wireless device, also bind the current access point.
  const auto deviceType = getPropertyOr<std::uint32_t>(*m_activeDevice, kNmDeviceInterface, "DeviceType", 0U);
  if (deviceType == kNmDeviceTypeWifi) {
    std::string apPath;
    try {
      const sdbus::Variant value =
          m_activeDevice->getProperty("ActiveAccessPoint").onInterface(kNmDeviceWirelessInterface);
      apPath = value.get<sdbus::ObjectPath>();
    } catch (const sdbus::Error&) {
    }
    rebindActiveAccessPoint(apPath);
  }
}

void NetworkManagerService::rebindActiveAccessPoint(const std::string& apPath) {
  if (apPath == m_activeApPath && m_activeAp != nullptr) {
    return;
  }
  m_activeApPath = apPath;
  m_activeAp.reset();
  if (apPath.empty() || apPath == "/") {
    return;
  }
  try {
    m_activeAp = sdbus::createProxy(m_bus.connection(), kNmBusName, sdbus::ObjectPath{apPath});
    m_activeAp->uponSignal("PropertiesChanged")
        .onInterface(kPropertiesInterface)
        .call([this](
                  const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& changedProperties,
                  const std::vector<std::string>& /*invalidatedProperties*/
              ) {
          if (interfaceName != kNmAccessPointInterface) {
            return;
          }
          if (changedProperties.contains("Strength") || changedProperties.contains("Ssid")) {
            refresh();
          }
        });
  } catch (const sdbus::Error& e) {
    kLog.debug("AP proxy failed: {}", e.what());
    m_activeAp.reset();
  }
}

void NetworkManagerService::readStateAsync(std::function<void(NetworkState)> onComplete) {
  const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
  auto next = std::make_shared<NetworkState>();
  next->scanning = m_scanning;

  bool vpnFromList = false;
  for (const auto& vpn : m_vpnConnections) {
    if (vpn.active) {
      vpnFromList = true;
      break;
    }
  }

  const std::string activeConnectionPath = m_activeConnectionPath;
  const std::string activeDevicePath = m_activeDevicePath;
  const std::string activeApPath = m_activeApPath;

  auto finish = [lifetimeToken, next, vpnFromList, onComplete]() {
    if (lifetimeToken.expired()) {
      return;
    }
    if (!next->vpnActive && vpnFromList) {
      next->vpnActive = true;
      next->connected = true;
    }
    onComplete(std::move(*next));
  };

  auto readActiveAccessPoint = [this, lifetimeToken, next, finish, activeApPath]() {
    if (activeApPath.empty() || activeApPath == "/") {
      finish();
      return;
    }

    try {
      auto apProxy = std::shared_ptr<sdbus::IProxy>(
          sdbus::createProxy(m_bus.connection(), kNmBusName, sdbus::ObjectPath{activeApPath})
      );
      apProxy->callMethodAsync("GetAll")
          .onInterface(kPropertiesInterface)
          .withArguments(kNmAccessPointInterface)
          .uponReplyInvoke([lifetimeToken, next, finish, apProxy](
                               std::optional<sdbus::Error> apErr, std::map<std::string, sdbus::Variant> apProperties
                           ) {
            if (lifetimeToken.expired()) {
              return;
            }
            if (!apErr.has_value()) {
              if (auto ssidIt = apProperties.find("Ssid"); ssidIt != apProperties.end()) {
                try {
                  const auto ssidBytes = ssidIt->second.get<std::vector<std::uint8_t>>();
                  next->ssid.assign(ssidBytes.begin(), ssidBytes.end());
                } catch (const sdbus::Error&) {
                }
              }
              if (auto strengthIt = apProperties.find("Strength"); strengthIt != apProperties.end()) {
                try {
                  next->signalStrength = strengthIt->second.get<std::uint8_t>();
                } catch (const sdbus::Error&) {
                }
              }
            }
            finish();
          });
    } catch (const sdbus::Error&) {
      finish();
    }
  };

  auto readDeviceState = [this, lifetimeToken, next, finish, readActiveAccessPoint, activeDevicePath]() {
    if (activeDevicePath.empty() || activeDevicePath == "/") {
      finish();
      return;
    }

    try {
      auto deviceProxy = std::shared_ptr<sdbus::IProxy>(
          sdbus::createProxy(m_bus.connection(), kNmBusName, sdbus::ObjectPath{activeDevicePath})
      );
      deviceProxy->callMethodAsync("GetAll")
          .onInterface(kPropertiesInterface)
          .withArguments(kNmDeviceInterface)
          .uponReplyInvoke([this, lifetimeToken, next, finish, readActiveAccessPoint, deviceProxy](
                               std::optional<sdbus::Error> deviceErr,
                               std::map<std::string, sdbus::Variant> deviceProperties
                           ) {
            if (lifetimeToken.expired()) {
              return;
            }
            if (!deviceErr.has_value()) {
              std::uint32_t deviceType = 0U;
              if (auto typeIt = deviceProperties.find("DeviceType"); typeIt != deviceProperties.end()) {
                try {
                  deviceType = typeIt->second.get<std::uint32_t>();
                } catch (const sdbus::Error&) {
                }
              }

              if (auto ifaceIt = deviceProperties.find("Interface"); ifaceIt != deviceProperties.end()) {
                try {
                  next->interfaceName = ifaceIt->second.get<std::string>();
                } catch (const sdbus::Error&) {
                }
              }

              std::string ip4ConfigPath;
              if (auto ip4It = deviceProperties.find("Ip4Config"); ip4It != deviceProperties.end()) {
                try {
                  ip4ConfigPath = ip4It->second.get<sdbus::ObjectPath>();
                } catch (const sdbus::Error&) {
                }
              }

              if (deviceType == kNmDeviceTypeWifi) {
                next->kind = NetworkConnectivity::Wireless;
              } else {
                next->kind = NetworkConnectivity::Wired;
              }

              auto finishAfterIp4 = [lifetimeToken, finish, readActiveAccessPoint, deviceType]() {
                if (lifetimeToken.expired()) {
                  return;
                }
                if (deviceType == kNmDeviceTypeWifi) {
                  readActiveAccessPoint();
                } else {
                  finish();
                }
              };

              if (ip4ConfigPath.empty() || ip4ConfigPath == "/") {
                finishAfterIp4();
                return;
              }

              try {
                auto ip4Proxy = std::shared_ptr<sdbus::IProxy>(
                    sdbus::createProxy(m_bus.connection(), kNmBusName, sdbus::ObjectPath{ip4ConfigPath})
                );
                ip4Proxy->callMethodAsync("GetAll")
                    .onInterface(kPropertiesInterface)
                    .withArguments(k_nmIp4ConfigInterface)
                    .uponReplyInvoke([lifetimeToken, next, finishAfterIp4, ip4Proxy](
                                         std::optional<sdbus::Error> ip4Err,
                                         std::map<std::string, sdbus::Variant> ip4Properties
                                     ) {
                      if (lifetimeToken.expired()) {
                        return;
                      }
                      if (!ip4Err.has_value()) {
                        if (auto addressDataIt = ip4Properties.find("AddressData");
                            addressDataIt != ip4Properties.end()) {
                          try {
                            const auto addressData =
                                addressDataIt->second.get<std::vector<std::map<std::string, sdbus::Variant>>>();
                            for (const auto& entry : addressData) {
                              auto addressIt = entry.find("address");
                              if (addressIt == entry.end()) {
                                continue;
                              }
                              try {
                                next->ipv4 = addressIt->second.get<std::string>();
                              } catch (const sdbus::Error&) {
                              }
                              if (!next->ipv4.empty()) {
                                break;
                              }
                            }
                          } catch (const sdbus::Error&) {
                          }
                        }

                        if (next->ipv4.empty()) {
                          if (auto addressesIt = ip4Properties.find("Addresses"); addressesIt != ip4Properties.end()) {
                            try {
                              const auto addresses = addressesIt->second.get<std::vector<std::vector<std::uint32_t>>>();
                              if (!addresses.empty() && !addresses.front().empty()) {
                                next->ipv4 = ipv4FromUint(addresses.front().front());
                              }
                            } catch (const sdbus::Error&) {
                            }
                          }
                        }
                      }
                      finishAfterIp4();
                    });
                return;
              } catch (const sdbus::Error&) {
              }

              finishAfterIp4();
              return;
            }

            finish();
          });
    } catch (const sdbus::Error&) {
      finish();
    }
  };

  auto readActiveConnectionState = [this, lifetimeToken, next, finish, readDeviceState, activeConnectionPath]() {
    if (activeConnectionPath.empty() || activeConnectionPath == "/") {
      readDeviceState();
      return;
    }

    try {
      auto connectionProxy = std::shared_ptr<sdbus::IProxy>(
          sdbus::createProxy(m_bus.connection(), kNmBusName, sdbus::ObjectPath{activeConnectionPath})
      );
      connectionProxy->callMethodAsync("GetAll")
          .onInterface(kPropertiesInterface)
          .withArguments(kNmActiveConnectionInterface)
          .uponReplyInvoke([lifetimeToken, next, readDeviceState, connectionProxy](
                               std::optional<sdbus::Error> connErr,
                               std::map<std::string, sdbus::Variant> connectionProperties
                           ) {
            if (lifetimeToken.expired()) {
              return;
            }
            if (!connErr.has_value()) {
              std::string type;
              if (auto typeIt = connectionProperties.find("Type"); typeIt != connectionProperties.end()) {
                try {
                  type = typeIt->second.get<std::string>();
                } catch (const sdbus::Error&) {
                }
              }

              std::uint32_t state = 0U;
              if (auto stateIt = connectionProperties.find("State"); stateIt != connectionProperties.end()) {
                try {
                  state = stateIt->second.get<std::uint32_t>();
                } catch (const sdbus::Error&) {
                }
              }

              next->vpnActive = (type == "vpn" || type == "wireguard");
              next->connected = state == kNmActiveConnectionStateActivated;
            }

            readDeviceState();
          });
    } catch (const sdbus::Error&) {
      readDeviceState();
    }
  };

  try {
    m_nm->callMethodAsync("GetAll")
        .onInterface(kPropertiesInterface)
        .withArguments(kNmInterface)
        .uponReplyInvoke([lifetimeToken, next, readActiveConnectionState](
                             std::optional<sdbus::Error> nmErr, std::map<std::string, sdbus::Variant> nmProperties
                         ) {
          if (lifetimeToken.expired()) {
            return;
          }
          if (!nmErr.has_value()) {
            if (auto wirelessEnabledIt = nmProperties.find("WirelessEnabled");
                wirelessEnabledIt != nmProperties.end()) {
              try {
                next->wirelessEnabled = wirelessEnabledIt->second.get<bool>();
              } catch (const sdbus::Error&) {
              }
            }
          }
          readActiveConnectionState();
        });
  } catch (const sdbus::Error&) {
    readActiveConnectionState();
  }
}

NetworkChangeOrigin NetworkManagerService::consumeWirelessEnabledChangeOrigin(bool enabled) {
  if (!m_pendingLocalWirelessEnabled.has_value()) {
    return NetworkChangeOrigin::External;
  }
  const bool matchesLocalRequest = *m_pendingLocalWirelessEnabled == enabled;
  m_pendingLocalWirelessEnabled.reset();
  return matchesLocalRequest ? NetworkChangeOrigin::Noctalia : NetworkChangeOrigin::External;
}

void NetworkManagerService::emitChangedIfNeeded(NetworkState next) {
  if (next == m_state) {
    return;
  }
  const bool wirelessEnabledChanged = next.wirelessEnabled != m_state.wirelessEnabled;
  const NetworkChangeOrigin origin =
      wirelessEnabledChanged ? consumeWirelessEnabledChangeOrigin(next.wirelessEnabled) : NetworkChangeOrigin::External;
  m_state = std::move(next);
  if (m_changeCallback) {
    m_changeCallback(m_state, origin);
  }
}
