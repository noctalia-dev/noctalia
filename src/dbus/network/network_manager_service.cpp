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
  constexpr auto kNmVpnConnectionInterface = "org.freedesktop.NetworkManager.VPN.Connection";
  constexpr auto kNmAccessPointInterface = "org.freedesktop.NetworkManager.AccessPoint";
  constexpr auto k_nmIp4ConfigInterface = "org.freedesktop.NetworkManager.IP4Config";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";

  using ConnectionSettings = std::map<std::string, std::map<std::string, sdbus::Variant>>;
  using VariantMap = std::map<std::string, sdbus::Variant>;
  constexpr std::string_view kNmWiredConnectionType = "802-3-ethernet";
  constexpr std::string_view kNmVpnConnectionType = "vpn";
  constexpr std::string_view kNmWireguardConnectionType = "wireguard";

  // NMDeviceType values from NetworkManager D-Bus API.
  constexpr std::uint32_t kNmDeviceTypeEthernet = 1;
  constexpr std::uint32_t kNmDeviceTypeWifi = 2;
  // Aggregating/virtual links that carry a wired L3 connection (a default-route
  // bridge/bond is the user's real LAN link, shown as wired).
  constexpr std::uint32_t kNmDeviceTypeBond = 10;
  constexpr std::uint32_t kNmDeviceTypeVlan = 11;
  constexpr std::uint32_t kNmDeviceTypeBridge = 13;
  constexpr std::uint32_t kNmDeviceTypeTeam = 15;

  // NMDeviceState: a device between Prepare and Activated is mid-activation.
  constexpr std::uint32_t kNmDeviceStatePrepare = 40;
  constexpr std::uint32_t kNmDeviceStateActivated = 100;

  // NMActiveConnectionState
  constexpr std::uint32_t kNmActiveConnectionStateActivating = 1;
  constexpr std::uint32_t kNmActiveConnectionStateActivated = 2;
  constexpr std::uint32_t kNmActiveConnectionStateDeactivated = 4;

  // NMSettingsConnectionFlags / NMSettingsUpdate2Flags.
  constexpr std::uint32_t kNmSettingsConnectionFlagUnsaved = 0x01;
  constexpr std::uint32_t k_nmSettingsUpdate2FlagToDisk = 0x01;

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
    std::vector<std::string> capturedWired;
    int pendingOps = 0;
    std::function<void()> onAllComplete;
  };

  struct SavedConnectionsState {
    std::vector<std::string> ssids;
    std::vector<std::string> wiredConnectionPaths;
    int pending = 0;
  };

  struct VpnRefreshState {
    std::vector<VpnConnectionInfo> vpns;
    std::set<std::string> vpnPaths;
    int pending = 0;
  };

  struct ActiveVpnState {
    std::set<std::string> activeProfilePaths;    // profiles activating or activated
    std::set<std::string> activatedProfilePaths; // profiles fully activated only
    std::set<std::string> vpnActivePaths;        // active-connection object paths belonging to VPN profiles
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

  // Best physical (ethernet/wifi) device with an active connection found so far.
  struct PhysicalPrimaryScan {
    std::string connectionPath;
    std::string devicePath;
    int score = 0;
    int pending = 0;
    std::function<void(std::string, std::string)> done;
  };

  struct WifiDeviceScan {
    std::vector<std::string> devicePaths;
    std::int64_t lastScanBaseline = 0;
    int pending = 0;
    std::function<void(std::vector<std::string>, std::int64_t)> done;
  };

  struct VpnDeactivateLookup {
    bool dispatched = false;
    int pending = 0;
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
        if (changedProperties.contains("PrimaryConnection")
            || changedProperties.contains("ActiveConnections")
            || changedProperties.contains("WirelessEnabled")
            || changedProperties.contains("State")
            || changedProperties.contains("Connectivity")) {
          requestRebind();
        }
        if (wirelessNowOn) {
          // NM powered the radio on but the wifi device is still transitioning
          // out of Unavailable, so calling RequestScan now would be rejected.
          // NM starts its own scan as soon as the device reaches Disconnected;
          // just mark ourselves scanning and snapshot LastScan so the device
          // PropertiesChanged watcher clears the flag when the scan finishes.
          collectWifiDevices([this](std::vector<std::string> devicePaths, std::int64_t lastScanBaseline) {
            if (devicePaths.empty()) {
              return;
            }
            m_scanning = true;
            m_scanBaselineLastScan = lastScanBaseline;
            refresh();
          });
        }
      });

  requestRebind();
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
  pending->capturedWired = m_savedWiredConnectionPaths;
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
      const bool wiredChanged = pending->capturedWired != m_savedWiredConnectionPaths;
      const bool stateChanged = next != m_state;
      const bool firstSnapshot = !m_hasStateSnapshot;
      const bool wirelessEnabledChanged = next.wirelessEnabled != m_state.wirelessEnabled;
      const NetworkChangeOrigin origin = wirelessEnabledChanged
          ? consumeWirelessEnabledChangeOrigin(next.wirelessEnabled)
          : NetworkChangeOrigin::External;
      // User actions force an emit so the UI always observes their completion,
      // even when the resulting state is unchanged (e.g. a failed activation).
      const bool forceEmit = m_emitOnNextRefresh;
      m_emitOnNextRefresh = false;
      m_state = std::move(next);
      m_hasStateSnapshot = true;
      if ((firstSnapshot || stateChanged || apsChanged || vpnsChanged || savedChanged || wiredChanged || forceEmit)
          && m_changeCallback) {
        m_changeCallback(m_state, origin);
      }
      // Break the self-reference cycle: pending->onAllComplete captures pending.
      pending->onAllComplete = {};
      // Async reply context: safe to drop retired activation proxies here.
      m_retiredApActivations.clear();

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
  const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
  collectWifiDevices([this, lifetimeToken](std::vector<std::string> devicePaths, std::int64_t lastScanBaseline) {
    for (const auto& devicePath : devicePaths) {
      try {
        auto device = std::shared_ptr<sdbus::IProxy>(
            sdbus::createProxy(m_bus.connection(), kNmBusName, sdbus::ObjectPath{devicePath})
        );
        const std::map<std::string, sdbus::Variant> options;
        device->callMethodAsync("RequestScan")
            .onInterface(kNmDeviceWirelessInterface)
            .withArguments(options)
            .uponReplyInvoke([this, lifetimeToken, device, devicePath,
                              lastScanBaseline](std::optional<sdbus::Error> err) {
              if (lifetimeToken.expired()) {
                return;
              }
              if (err.has_value()) {
                kLog.debug("RequestScan failed on {}: {}", devicePath, err->what());
                return;
              }
              if (!m_scanning) {
                m_scanning = true;
                m_scanBaselineLastScan = lastScanBaseline;
                refresh();
              }
            });
      } catch (const sdbus::Error& e) {
        kLog.debug("RequestScan dispatch failed on {}: {}", devicePath, e.what());
      }
    }
  });
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
    const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
    try {
      m_nm->callMethodAsync("ActivateConnection")
          .onInterface(kNmInterface)
          .withArguments(sdbus::ObjectPath{"/"}, sdbus::ObjectPath{ap.devicePath}, sdbus::ObjectPath{ap.path})
          .uponReplyInvoke([this, lifetimeToken, ap](std::optional<sdbus::Error> err, sdbus::ObjectPath activePath) {
            if (lifetimeToken.expired()) {
              return;
            }
            if (err.has_value()) {
              kLog.debug("ActivateConnection(/) failed for ssid={}: {}; trying AddAndActivate", ap.ssid, err->what());
              if (!ap.secured) {
                addAndActivateAccessPoint(ap, std::nullopt);
              } else {
                m_emitOnNextRefresh = true;
                refresh();
              }
              return;
            }
            kLog.info("activating ap ssid={} active={}", ap.ssid, std::string(activePath));
            m_emitOnNextRefresh = true;
            refresh();
          });
      return true;
    } catch (const sdbus::Error& e) {
      kLog.debug("ActivateConnection(/) dispatch failed for ssid={}: {}", ap.ssid, e.what());
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
  const std::string ssid = ap.ssid;
  const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;

  auto onActivated = [this, ssid](const std::string& connectionPath, const std::string& activePath) {
    kLog.info("add+activate ap ssid={} conn={} active={}", ssid, connectionPath, activePath);
    watchPendingAccessPointActivation(ssid, connectionPath, activePath);
    m_emitOnNextRefresh = true;
    refresh();
  };

  auto fallback = [this, lifetimeToken, settings, devicePath, apPath, ssid, onActivated]() {
    try {
      m_nm->callMethodAsync("AddAndActivateConnection")
          .onInterface(kNmInterface)
          .withArguments(settings, devicePath, apPath)
          .uponReplyInvoke([lifetimeToken, ssid, onActivated](
                               std::optional<sdbus::Error> err, sdbus::ObjectPath connectionPath,
                               sdbus::ObjectPath activePath
                           ) {
            if (lifetimeToken.expired()) {
              return;
            }
            if (err.has_value()) {
              kLog.warn("AddAndActivateConnection failed ssid={} err={}", ssid, err->what());
              return;
            }
            onActivated(connectionPath, activePath);
          });
    } catch (const sdbus::Error& e) {
      kLog.warn("AddAndActivateConnection dispatch failed ssid={} err={}", ssid, e.what());
    }
  };

  try {
    const VariantMap options{{"persist", sdbus::Variant{std::string("memory")}}};
    m_nm->callMethodAsync("AddAndActivateConnection2")
        .onInterface(kNmInterface)
        .withArguments(settings, devicePath, apPath, options)
        .uponReplyInvoke([lifetimeToken, ssid, onActivated, fallback](
                             std::optional<sdbus::Error> err, sdbus::ObjectPath connectionPath,
                             sdbus::ObjectPath activePath, VariantMap /*result*/
                         ) {
          if (lifetimeToken.expired()) {
            return;
          }
          if (err.has_value()) {
            if (err->getName() == sdbus::Error::Name{"org.freedesktop.DBus.Error.UnknownMethod"}) {
              kLog.debug(
                  "AddAndActivateConnection2 unavailable for ssid={}; falling back to AddAndActivateConnection", ssid
              );
              fallback();
            } else {
              kLog.warn("AddAndActivateConnection2 failed ssid={} err={}", ssid, err->what());
            }
            return;
          }
          onActivated(connectionPath, activePath);
        });
    return true;
  } catch (const sdbus::Error& e) {
    kLog.warn("AddAndActivateConnection2 dispatch failed ssid={} err={}", ssid, e.what());
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
    activeProxy->callMethodAsync("Get")
        .onInterface(kPropertiesInterface)
        .withArguments(kNmActiveConnectionInterface, "State")
        .uponReplyInvoke([this, lifetimeToken, activePath](std::optional<sdbus::Error> err, sdbus::Variant value) {
          if (lifetimeToken.expired() || err.has_value()) {
            return;
          }
          try {
            handlePendingAccessPointActivationState(activePath, value.get<std::uint32_t>());
          } catch (const sdbus::Error&) {
          }
        });
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
  if (state != kNmActiveConnectionStateActivated && state != kNmActiveConnectionStateDeactivated) {
    return;
  }
  const std::string ssid = it->second->ssid;
  const std::string connectionPath = it->second->connectionPath;
  // We may be inside this activation proxy's own signal/reply handler, so its
  // destruction is deferred to the next refresh completion.
  m_retiredApActivations.push_back(std::move(it->second));
  m_pendingApActivations.erase(it);
  if (state == kNmActiveConnectionStateActivated) {
    kLog.info("ap activation succeeded ssid={} conn={}", ssid, connectionPath);
    persistConnectionToDisk(connectionPath, ssid);
  } else {
    kLog.info("ap activation did not complete ssid={} conn={}", ssid, connectionPath);
    deleteUnsavedConnection(connectionPath, ssid);
  }
  refresh();
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
          m_emitOnNextRefresh = true;
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
  const std::string vpnPath = vpn.path;
  const std::string vpnName = vpn.name;
  const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
  try {
    m_nm->callMethodAsync("Get")
        .onInterface(kPropertiesInterface)
        .withArguments(kNmInterface, "ActiveConnections")
        .uponReplyInvoke([this, lifetimeToken, vpnPath,
                          vpnName](std::optional<sdbus::Error> err, sdbus::Variant activeListValue) {
          if (lifetimeToken.expired()) {
            return;
          }
          std::vector<sdbus::ObjectPath> activePaths;
          if (!err.has_value()) {
            try {
              activePaths = activeListValue.get<std::vector<sdbus::ObjectPath>>();
            } catch (const sdbus::Error&) {
            }
          }
          if (activePaths.empty()) {
            kLog.debug("DeactivateConnection(vpn): no active connections name={}", vpnName);
            m_emitOnNextRefresh = true;
            refresh();
            return;
          }

          auto lookup = std::make_shared<VpnDeactivateLookup>();
          lookup->pending = static_cast<int>(activePaths.size());

          auto onLookupComplete = [this, lifetimeToken, lookup, vpnName]() {
            if (lifetimeToken.expired()) {
              return;
            }
            if (--lookup->pending == 0 && !lookup->dispatched) {
              kLog.debug("DeactivateConnection(vpn): no matching active connection name={}", vpnName);
              m_emitOnNextRefresh = true;
              refresh();
            }
          };

          for (const auto& activePath : activePaths) {
            try {
              auto active =
                  std::shared_ptr<sdbus::IProxy>(sdbus::createProxy(m_bus.connection(), kNmBusName, activePath));
              const std::string activePathStr{activePath};
              active->callMethodAsync("GetAll")
                  .onInterface(kPropertiesInterface)
                  .withArguments(kNmActiveConnectionInterface)
                  .uponReplyInvoke(
                      [this, lifetimeToken, active, lookup, activePathStr, vpnPath, vpnName, onLookupComplete](
                          std::optional<sdbus::Error> getAllErr, std::map<std::string, sdbus::Variant> properties
                      ) {
                        if (lifetimeToken.expired()) {
                          return;
                        }
                        if (!getAllErr.has_value() && !lookup->dispatched) {
                          std::string profilePath;
                          if (auto connIt = properties.find("Connection"); connIt != properties.end()) {
                            try {
                              profilePath = connIt->second.get<sdbus::ObjectPath>();
                            } catch (const sdbus::Error&) {
                            }
                          }
                          std::uint32_t state = 0U;
                          if (auto stateIt = properties.find("State"); stateIt != properties.end()) {
                            try {
                              state = stateIt->second.get<std::uint32_t>();
                            } catch (const sdbus::Error&) {
                            }
                          }
                          // Also abort a connection stuck activating, otherwise a VPN
                          // that lost its link can never be turned off from the UI.
                          const bool deactivatable =
                              state == kNmActiveConnectionStateActivated || state == kNmActiveConnectionStateActivating;
                          if (profilePath == vpnPath && deactivatable) {
                            lookup->dispatched = true;
                            try {
                              m_nm->callMethodAsync("DeactivateConnection")
                                  .onInterface(kNmInterface)
                                  .withArguments(sdbus::ObjectPath{activePathStr})
                                  .uponReplyInvoke([this, lifetimeToken, activePathStr,
                                                    vpnName](std::optional<sdbus::Error> deactivateErr) {
                                    if (lifetimeToken.expired()) {
                                      return;
                                    }
                                    if (deactivateErr.has_value()) {
                                      kLog.warn(
                                          "DeactivateConnection(vpn) failed name={} active={}: {}", vpnName,
                                          activePathStr, deactivateErr->what()
                                      );
                                    } else {
                                      kLog.info("deactivated vpn name={} active={}", vpnName, activePathStr);
                                    }
                                    m_emitOnNextRefresh = true;
                                    refresh();
                                  });
                            } catch (const sdbus::Error& e) {
                              kLog.warn(
                                  "DeactivateConnection(vpn) dispatch failed name={} active={}: {}", vpnName,
                                  activePathStr, e.what()
                              );
                            }
                          }
                        }
                        onLookupComplete();
                      }
                  );
            } catch (const sdbus::Error&) {
              onLookupComplete();
            }
          }
        });
    return true;
  } catch (const sdbus::Error& e) {
    kLog.warn("DeactivateConnection(vpn) lookup dispatch failed path={}: {}", vpn.path, e.what());
    return false;
  }
}

bool NetworkManagerService::canActivateWiredConnection() const noexcept { return !m_savedWiredConnectionPaths.empty(); }

bool NetworkManagerService::activateWiredConnection() {
  if (m_state.kind == NetworkConnectivity::Wired && m_state.connected) {
    return true;
  }
  if (m_savedWiredConnectionPaths.empty()) {
    return false;
  }
  // The saved list is sorted by object path, which says nothing about which
  // profile can actually activate — try each in turn until one succeeds.
  auto candidates = std::make_shared<std::vector<std::string>>(m_savedWiredConnectionPaths);
  tryActivateWiredConnection(std::move(candidates), 0);
  return true;
}

void NetworkManagerService::tryActivateWiredConnection(
    std::shared_ptr<std::vector<std::string>> candidates, std::size_t index
) {
  if (index >= candidates->size()) {
    kLog.warn("ActivateConnection(wired) failed for all {} saved profiles", candidates->size());
    m_emitOnNextRefresh = true;
    refresh();
    return;
  }
  const std::string connectionPath = (*candidates)[index];
  const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
  try {
    m_nm->callMethodAsync("ActivateConnection")
        .onInterface(kNmInterface)
        .withArguments(sdbus::ObjectPath{connectionPath}, sdbus::ObjectPath{"/"}, sdbus::ObjectPath{"/"})
        .uponReplyInvoke([this, lifetimeToken, candidates, index,
                          connectionPath](std::optional<sdbus::Error> err, sdbus::ObjectPath activePath) {
          if (lifetimeToken.expired()) {
            return;
          }
          if (err.has_value()) {
            // Typical error: "no suitable device" for a profile whose NIC is absent.
            kLog.warn("ActivateConnection(wired) failed path={}: {}", connectionPath, err->what());
            tryActivateWiredConnection(candidates, index + 1);
            return;
          }
          kLog.info("activating wired connection path={} active={}", connectionPath, std::string(activePath));
          m_emitOnNextRefresh = true;
          requestRebind();
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("ActivateConnection(wired) dispatch failed path={}: {}", connectionPath, e.what());
    tryActivateWiredConnection(candidates, index + 1);
  }
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
  // Async: the write is polkit-gated; a sync call can block the main loop
  // while authorization is pending.
  const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
  try {
    m_nm->setPropertyAsync("WirelessEnabled")
        .onInterface(kNmInterface)
        .toValue(enabled)
        .uponReplyInvoke([this, lifetimeToken, enabled](std::optional<sdbus::Error> err) {
          if (lifetimeToken.expired() || !err.has_value()) {
            return;
          }
          if (m_pendingLocalWirelessEnabled == enabled) {
            m_pendingLocalWirelessEnabled.reset();
          }
          kLog.warn("WirelessEnabled write failed: {}", err->what());
        });
  } catch (const sdbus::Error& e) {
    if (m_pendingLocalWirelessEnabled == enabled) {
      m_pendingLocalWirelessEnabled.reset();
    }
    kLog.warn("WirelessEnabled write dispatch failed: {}", e.what());
  }
}

void NetworkManagerService::disconnect() {
  if (m_state.kind == NetworkConnectivity::Wired && !m_activeDevicePath.empty() && m_activeDevicePath != "/") {
    // DeactivateConnection can be immediately undone by a wired profile's
    // autoconnect policy. Device.Disconnect keeps the device down until the
    // user manually activates it again.
    const std::string devicePath = m_activeDevicePath;
    const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
    try {
      auto device = std::shared_ptr<sdbus::IProxy>(
          sdbus::createProxy(m_bus.connection(), kNmBusName, sdbus::ObjectPath{devicePath})
      );
      device->callMethodAsync("Disconnect")
          .onInterface(kNmDeviceInterface)
          .uponReplyInvoke([this, lifetimeToken, device, devicePath](std::optional<sdbus::Error> err) {
            if (lifetimeToken.expired()) {
              return;
            }
            if (err.has_value()) {
              kLog.warn("Device.Disconnect failed path={}: {}", devicePath, err->what());
            } else {
              kLog.info("disconnected wired device path={}", devicePath);
            }
            m_emitOnNextRefresh = true;
            requestRebind();
          });
      return;
    } catch (const sdbus::Error& e) {
      kLog.warn("Device.Disconnect dispatch failed path={}: {}", devicePath, e.what());
    }
  }

  if (m_activeConnectionPath.empty() || m_activeConnectionPath == "/") {
    return;
  }
  // Async: DeactivateConnection on a system-owned profile is gated by polkit,
  // and a sync call would freeze the main loop while the polkit agent prompts
  // (or while polkit waits for an agent to register). Fire-and-forget here.
  const std::string activePath = m_activeConnectionPath;
  const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
  try {
    m_nm->callMethodAsync("DeactivateConnection")
        .onInterface(kNmInterface)
        .withArguments(sdbus::ObjectPath{activePath})
        .uponReplyInvoke([this, lifetimeToken, activePath](std::optional<sdbus::Error> err) {
          if (lifetimeToken.expired()) {
            return;
          }
          if (err.has_value()) {
            kLog.warn("DeactivateConnection failed path={}: {}", activePath, err->what());
          } else {
            kLog.info("deactivated connection path={}", activePath);
          }
          m_emitOnNextRefresh = true;
          requestRebind();
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
  return std::ranges::contains(m_savedSsids, ssid);
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
            m_savedWiredConnectionPaths.clear();
            onComplete();
            return;
          }

          auto savedState = std::make_shared<SavedConnectionsState>();
          savedState->pending = static_cast<int>(connectionPaths.size());

          auto finishOne = [this, savedState, onComplete]() {
            if (--savedState->pending == 0) {
              finishSavedConnections(savedState->ssids, savedState->wiredConnectionPaths, onComplete);
            }
          };

          for (const auto& connectionPath : connectionPaths) {
            try {
              auto connection =
                  std::shared_ptr<sdbus::IProxy>(sdbus::createProxy(m_bus.connection(), kNmBusName, connectionPath));
              connection->callMethodAsync("GetAll")
                  .onInterface(kPropertiesInterface)
                  .withArguments(kNmSettingsConnectionInterface)
                  .uponReplyInvoke([this, lifetimeToken, connection, savedState, connectionPath, onComplete,
                                    finishOne](std::optional<sdbus::Error> metaErr, VariantMap metaProps) {
                    if (lifetimeToken.expired()) {
                      return;
                    }
                    std::uint32_t flags = 0;
                    std::string filename;
                    if (!metaErr.has_value()) {
                      if (auto it = metaProps.find("Flags"); it != metaProps.end()) {
                        try {
                          flags = it->second.get<std::uint32_t>();
                        } catch (const sdbus::Error&) {
                        }
                      }
                      if (auto it = metaProps.find("Filename"); it != metaProps.end()) {
                        try {
                          filename = it->second.get<std::string>();
                        } catch (const sdbus::Error&) {
                        }
                      }
                    }
                    if (metaErr.has_value() || ((flags & kNmSettingsConnectionFlagUnsaved) != 0U && filename.empty())) {
                      finishOne();
                      return;
                    }
                    try {
                      connection->callMethodAsync("GetSettings")
                          .onInterface(kNmSettingsConnectionInterface)
                          .uponReplyInvoke([this, lifetimeToken, connection, savedState, connectionPath, onComplete](
                                               std::optional<sdbus::Error> settingsErr,
                                               std::map<std::string, std::map<std::string, sdbus::Variant>> cfg
                                           ) {
                            if (lifetimeToken.expired()) {
                              return;
                            }
                            if (!settingsErr.has_value()) {
                              auto connIt = cfg.find("connection");
                              if (connIt != cfg.end()) {
                                auto typeIt = connIt->second.find("type");
                                if (typeIt != connIt->second.end()) {
                                  try {
                                    const auto type = typeIt->second.get<std::string>();
                                    if (type == kNmWiredConnectionType) {
                                      savedState->wiredConnectionPaths.emplace_back(connectionPath);
                                    }
                                  } catch (const sdbus::Error&) {
                                  }
                                }
                              }

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
                              finishSavedConnections(savedState->ssids, savedState->wiredConnectionPaths, onComplete);
                            }
                          });
                    } catch (const sdbus::Error&) {
                      finishOne();
                    }
                  });
            } catch (const sdbus::Error&) {
              finishOne();
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
            m_anyVpnConnected = false;
            reconcileVpnActiveWatchers({});
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
                    m_anyVpnConnected = false;
                    reconcileVpnActiveWatchers({});
                    finalize();
                    return;
                  }

                  std::vector<sdbus::ObjectPath> activePaths;
                  try {
                    activePaths = activeListValue.get<std::vector<sdbus::ObjectPath>>();
                  } catch (const sdbus::Error&) {
                    m_anyVpnConnected = false;
                    reconcileVpnActiveWatchers({});
                    finalize();
                    return;
                  }

                  if (activePaths.empty()) {
                    m_anyVpnConnected = false;
                    reconcileVpnActiveWatchers({});
                    finalize();
                    return;
                  }

                  auto activeState = std::make_shared<ActiveVpnState>();
                  activeState->pending = static_cast<int>(activePaths.size());

                  auto onActiveComplete = [this, lifetimeToken, vpnState, activeState, finalize]() {
                    if (lifetimeToken.expired()) {
                      return;
                    }
                    if (--activeState->pending == 0) {
                      bool anyConnected = false;
                      for (auto& vpn : vpnState->vpns) {
                        if (activeState->activeProfilePaths.contains(vpn.path)) {
                          vpn.active = true;
                        }
                        if (activeState->activatedProfilePaths.contains(vpn.path)) {
                          anyConnected = true;
                        }
                      }
                      m_anyVpnConnected = anyConnected;
                      reconcileVpnActiveWatchers(activeState->vpnActivePaths);
                      finalize();
                    }
                  };

                  for (const auto& activePath : activePaths) {
                    try {
                      auto active = std::shared_ptr<sdbus::IProxy>(
                          sdbus::createProxy(m_bus.connection(), kNmBusName, activePath)
                      );
                      const std::string activePathStr{activePath};
                      active->callMethodAsync("GetAll")
                          .onInterface(kPropertiesInterface)
                          .withArguments(kNmActiveConnectionInterface)
                          .uponReplyInvoke([lifetimeToken, active, vpnState, activeState, activePathStr,
                                            onActiveComplete](
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

                              std::string profilePath;
                              if (auto connIt = properties.find("Connection"); connIt != properties.end()) {
                                try {
                                  profilePath = connIt->second.get<sdbus::ObjectPath>();
                                } catch (const sdbus::Error&) {
                                }
                              }

                              if (!profilePath.empty()) {
                                if (vpnState->vpnPaths.contains(profilePath)) {
                                  activeState->vpnActivePaths.insert(activePathStr);
                                }
                                if (state == kNmActiveConnectionStateActivating
                                    || state == kNmActiveConnectionStateActivated) {
                                  activeState->activeProfilePaths.insert(profilePath);
                                  if (state == kNmActiveConnectionStateActivated) {
                                    activeState->activatedProfilePaths.insert(profilePath);
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

void NetworkManagerService::reconcileVpnActiveWatchers(const std::set<std::string>& activePaths) {
  // VPN state transitions (Activating -> Activated, teardown) don't always move
  // PrimaryConnection, so watch each VPN active connection directly and refresh
  // when its state changes. Called from async reply context only — never from a
  // watcher's own signal handler — so erasing watchers here is safe.
  std::erase_if(m_vpnActiveWatchers, [&activePaths](const auto& entry) { return !activePaths.contains(entry.first); });
  for (const auto& activePath : activePaths) {
    if (m_vpnActiveWatchers.contains(activePath)) {
      continue;
    }
    try {
      auto proxy = sdbus::createProxy(m_bus.connection(), kNmBusName, sdbus::ObjectPath{activePath});
      proxy->uponSignal("PropertiesChanged")
          .onInterface(kPropertiesInterface)
          .call([this](
                    const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& changedProperties,
                    const std::vector<std::string>& /*invalidatedProperties*/
                ) {
            const bool activeStateChanged =
                interfaceName == kNmActiveConnectionInterface && changedProperties.contains("State");
            const bool vpnStateChanged =
                interfaceName == kNmVpnConnectionInterface && changedProperties.contains("VpnState");
            if (activeStateChanged || vpnStateChanged) {
              refresh();
            }
          });
      m_vpnActiveWatchers.emplace(activePath, std::move(proxy));
    } catch (const sdbus::Error& e) {
      kLog.debug("vpn active watcher failed {}: {}", activePath, e.what());
    }
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

void NetworkManagerService::collectWifiDevices(
    std::function<void(std::vector<std::string> devicePaths, std::int64_t lastScanBaseline)> done
) {
  const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
  try {
    m_nm->callMethodAsync("GetDevices")
        .onInterface(kNmInterface)
        .uponReplyInvoke([this, lifetimeToken,
                          done](std::optional<sdbus::Error> err, std::vector<sdbus::ObjectPath> devices) {
          if (lifetimeToken.expired()) {
            return;
          }
          if (err.has_value() || devices.empty()) {
            done({}, 0);
            return;
          }

          auto scan = std::make_shared<WifiDeviceScan>();
          scan->pending = static_cast<int>(devices.size());
          scan->done = done;

          for (const auto& devicePath : devices) {
            try {
              auto device =
                  std::shared_ptr<sdbus::IProxy>(sdbus::createProxy(m_bus.connection(), kNmBusName, devicePath));
              const std::string devicePathStr{devicePath};
              // GetAll on the wireless interface succeeds only for wifi devices.
              device->callMethodAsync("GetAll")
                  .onInterface(kPropertiesInterface)
                  .withArguments(kNmDeviceWirelessInterface)
                  .uponReplyInvoke([lifetimeToken, device, scan, devicePathStr](
                                       std::optional<sdbus::Error> wifiErr,
                                       std::map<std::string, sdbus::Variant> properties
                                   ) {
                    if (lifetimeToken.expired()) {
                      return;
                    }
                    if (!wifiErr.has_value()) {
                      scan->devicePaths.push_back(devicePathStr);
                      if (auto it = properties.find("LastScan"); it != properties.end()) {
                        try {
                          scan->lastScanBaseline = std::max(scan->lastScanBaseline, it->second.get<std::int64_t>());
                        } catch (const sdbus::Error&) {
                        }
                      }
                    }
                    if (--scan->pending == 0 && scan->done) {
                      scan->done(std::move(scan->devicePaths), scan->lastScanBaseline);
                    }
                  });
            } catch (const sdbus::Error&) {
              if (--scan->pending == 0 && scan->done) {
                scan->done(std::move(scan->devicePaths), scan->lastScanBaseline);
              }
            }
          }
        });
  } catch (const sdbus::Error& e) {
    kLog.debug("collectWifiDevices dispatch failed: {}", e.what());
    done({}, 0);
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

void NetworkManagerService::finishSavedConnections(
    std::vector<std::string>& ssids, std::vector<std::string>& wiredConnectionPaths, std::function<void()> onComplete
) {
  std::ranges::sort(ssids);
  ssids.erase(std::ranges::unique(ssids).begin(), ssids.end());
  m_savedSsids = std::move(ssids);

  std::ranges::sort(wiredConnectionPaths);
  wiredConnectionPaths.erase(std::ranges::unique(wiredConnectionPaths).begin(), wiredConnectionPaths.end());
  m_savedWiredConnectionPaths = std::move(wiredConnectionPaths);
  onComplete();
}

void NetworkManagerService::finishRefreshAccessPoints(
    std::vector<AccessPointInfo>& aps, std::function<void()> onComplete
) {
  // Deduplicate by SSID, keeping the strongest (and marking active if any entry is active).
  std::vector<AccessPointInfo> deduped;
  deduped.reserve(aps.size());
  for (auto& ap : aps) {
    auto it = std::ranges::find(deduped, ap.ssid, &AccessPointInfo::ssid);
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

void NetworkManagerService::requestRebind() {
  if (m_rebindInFlight) {
    m_rebindQueued = true;
    return;
  }
  m_rebindInFlight = true;
  const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
  try {
    m_nm->callMethodAsync("Get")
        .onInterface(kPropertiesInterface)
        .withArguments(kNmInterface, "PrimaryConnection")
        .uponReplyInvoke([this, lifetimeToken](std::optional<sdbus::Error> err, sdbus::Variant value) {
          if (lifetimeToken.expired()) {
            return;
          }
          std::string primaryPath;
          if (!err.has_value()) {
            try {
              primaryPath = value.get<sdbus::ObjectPath>();
            } catch (const sdbus::Error&) {
            }
          }
          // PrimaryConnection stays "/" until a connection finishes activating,
          // and it points at the VPN when one holds the default route. In both
          // cases resolve the physical ethernet/wifi link instead, so the state
          // (and disconnect target) always describe the physical connection.
          if (primaryPath.empty() || primaryPath == "/") {
            resolvePhysicalPrimary(false, [this](std::string connectionPath, std::string devicePath) {
              adoptActiveConnection(connectionPath, devicePath);
            });
            return;
          }
          try {
            auto primary = std::shared_ptr<sdbus::IProxy>(
                sdbus::createProxy(m_bus.connection(), kNmBusName, sdbus::ObjectPath{primaryPath})
            );
            primary->callMethodAsync("GetAll")
                .onInterface(kPropertiesInterface)
                .withArguments(kNmActiveConnectionInterface)
                .uponReplyInvoke([this, lifetimeToken, primary, primaryPath](
                                     std::optional<sdbus::Error> getAllErr,
                                     std::map<std::string, sdbus::Variant> properties
                                 ) {
                  if (lifetimeToken.expired()) {
                    return;
                  }
                  std::string type;
                  std::string devicePath;
                  if (!getAllErr.has_value()) {
                    if (auto typeIt = properties.find("Type"); typeIt != properties.end()) {
                      try {
                        type = typeIt->second.get<std::string>();
                      } catch (const sdbus::Error&) {
                      }
                    }
                    if (auto devIt = properties.find("Devices"); devIt != properties.end()) {
                      try {
                        const auto devices = devIt->second.get<std::vector<sdbus::ObjectPath>>();
                        if (!devices.empty()) {
                          devicePath = devices.front();
                        }
                      } catch (const sdbus::Error&) {
                      }
                    }
                  }
                  if (type == kNmVpnConnectionType || type == kNmWireguardConnectionType) {
                    // A VPN holding the default route must not masquerade as the
                    // physical link; describe the ethernet/wifi device beneath it.
                    resolvePhysicalPrimary(true, [this](std::string connectionPath, std::string physicalDevicePath) {
                      adoptActiveConnection(connectionPath, physicalDevicePath);
                    });
                  } else {
                    // Any real default-route link (wired, wireless, bridge, bond, …)
                    // is itself the connection to describe.
                    adoptActiveConnection(primaryPath, devicePath);
                  }
                });
          } catch (const sdbus::Error&) {
            resolvePhysicalPrimary(true, [this](std::string connectionPath, std::string devicePath) {
              adoptActiveConnection(connectionPath, devicePath);
            });
          }
        });
  } catch (const sdbus::Error& e) {
    kLog.debug("requestRebind dispatch failed: {}", e.what());
    m_rebindInFlight = false;
    refresh();
  }
}

void NetworkManagerService::resolvePhysicalPrimary(
    bool allowActivatedAsPrimary, std::function<void(std::string connectionPath, std::string devicePath)> done
) {
  const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
  try {
    m_nm->callMethodAsync("GetDevices")
        .onInterface(kNmInterface)
        .uponReplyInvoke([this, lifetimeToken, allowActivatedAsPrimary,
                          done](std::optional<sdbus::Error> err, std::vector<sdbus::ObjectPath> devices) {
          if (lifetimeToken.expired()) {
            return;
          }
          if (err.has_value() || devices.empty()) {
            done({}, {});
            return;
          }

          auto scan = std::make_shared<PhysicalPrimaryScan>();
          scan->pending = static_cast<int>(devices.size());
          scan->done = done;

          for (const auto& devicePath : devices) {
            try {
              auto device =
                  std::shared_ptr<sdbus::IProxy>(sdbus::createProxy(m_bus.connection(), kNmBusName, devicePath));
              const std::string devicePathStr{devicePath};
              device->callMethodAsync("GetAll")
                  .onInterface(kPropertiesInterface)
                  .withArguments(kNmDeviceInterface)
                  .uponReplyInvoke([lifetimeToken, device, scan, devicePathStr, allowActivatedAsPrimary](
                                       std::optional<sdbus::Error> devErr,
                                       std::map<std::string, sdbus::Variant> properties
                                   ) {
                    if (lifetimeToken.expired()) {
                      return;
                    }
                    if (!devErr.has_value()) {
                      std::uint32_t deviceType = 0U;
                      std::uint32_t state = 0U;
                      std::string activePath;
                      if (auto it = properties.find("DeviceType"); it != properties.end()) {
                        try {
                          deviceType = it->second.get<std::uint32_t>();
                        } catch (const sdbus::Error&) {
                        }
                      }
                      if (auto it = properties.find("State"); it != properties.end()) {
                        try {
                          state = it->second.get<std::uint32_t>();
                        } catch (const sdbus::Error&) {
                        }
                      }
                      if (auto it = properties.find("ActiveConnection"); it != properties.end()) {
                        try {
                          activePath = it->second.get<sdbus::ObjectPath>();
                        } catch (const sdbus::Error&) {
                        }
                      }
                      const bool physical = deviceType == kNmDeviceTypeEthernet || deviceType == kNmDeviceTypeWifi;
                      if (physical && !activePath.empty() && activePath != "/") {
                        // Prefer activated over activating, ethernet over wifi.
                        // An activated device only counts as the connected primary
                        // once NM has an established default route; otherwise it may
                        // be a bridge/bond slave that activates long before the link
                        // it feeds is usable.
                        int score = 0;
                        if (allowActivatedAsPrimary && state == kNmDeviceStateActivated) {
                          score = 4;
                        } else if (state >= kNmDeviceStatePrepare && state < kNmDeviceStateActivated) {
                          score = 2;
                        }
                        if (score > 0 && deviceType == kNmDeviceTypeEthernet) {
                          ++score;
                        }
                        if (score > scan->score) {
                          scan->score = score;
                          scan->connectionPath = activePath;
                          scan->devicePath = devicePathStr;
                        }
                      }
                    }
                    if (--scan->pending == 0 && scan->done) {
                      scan->done(scan->connectionPath, scan->devicePath);
                    }
                  });
            } catch (const sdbus::Error&) {
              if (--scan->pending == 0 && scan->done) {
                scan->done(scan->connectionPath, scan->devicePath);
              }
            }
          }
        });
  } catch (const sdbus::Error& e) {
    kLog.debug("resolvePhysicalPrimary dispatch failed: {}", e.what());
    done({}, {});
  }
}

void NetworkManagerService::adoptActiveConnection(const std::string& connectionPath, const std::string& devicePath) {
  const bool valid = !connectionPath.empty() && connectionPath != "/";
  const std::string normalized = valid ? connectionPath : std::string{};
  if (normalized != m_activeConnectionPath) {
    m_activeConnectionPath = normalized;
    // Safe: adoptActiveConnection only runs in async reply context, never
    // inside this proxy's own signal handler.
    m_activeConnection.reset();
    if (valid) {
      try {
        m_activeConnection = sdbus::createProxy(m_bus.connection(), kNmBusName, sdbus::ObjectPath{normalized});
        m_activeConnection->uponSignal("PropertiesChanged")
            .onInterface(kPropertiesInterface)
            .call([this](
                      const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& changedProperties,
                      const std::vector<std::string>& /*invalidatedProperties*/
                  ) {
              if (interfaceName != kNmActiveConnectionInterface) {
                return;
              }
              if (changedProperties.contains("Devices")
                  || changedProperties.contains("State")
                  || changedProperties.contains("Ip4Config")) {
                requestRebind();
              }
            });
      } catch (const sdbus::Error& e) {
        kLog.debug("active connection proxy failed: {}", e.what());
        m_activeConnection.reset();
      }
    }
  }
  rebindActiveDevice(devicePath);

  m_rebindInFlight = false;
  refresh();
  if (m_rebindQueued) {
    m_rebindQueued = false;
    requestRebind();
  }
}

void NetworkManagerService::rebindActiveDevice(const std::string& devicePath) {
  const std::string normalized = (devicePath.empty() || devicePath == "/") ? std::string{} : devicePath;
  if (normalized == m_activeDevicePath && (normalized.empty() || m_activeDevice != nullptr)) {
    return;
  }
  m_activeDevicePath = normalized;
  m_activeDevice.reset();
  rebindActiveAccessPoint({});

  if (normalized.empty()) {
    return;
  }

  try {
    m_activeDevice = sdbus::createProxy(m_bus.connection(), kNmBusName, sdbus::ObjectPath{normalized});
    m_activeDevice->uponSignal("PropertiesChanged")
        .onInterface(kPropertiesInterface)
        .call([this](
                  const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& changedProperties,
                  const std::vector<std::string>& /*invalidatedProperties*/
              ) {
          if (interfaceName == kNmDeviceInterface) {
            if (changedProperties.contains("Ip4Config")
                || changedProperties.contains("State")
                || changedProperties.contains("Interface")) {
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

  // Wireless probe: GetAll on the wireless interface succeeds only for wifi
  // devices and carries ActiveAccessPoint, so no DeviceType read is needed.
  const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
  try {
    auto probe = std::shared_ptr<sdbus::IProxy>(
        sdbus::createProxy(m_bus.connection(), kNmBusName, sdbus::ObjectPath{normalized})
    );
    probe->callMethodAsync("GetAll")
        .onInterface(kPropertiesInterface)
        .withArguments(kNmDeviceWirelessInterface)
        .uponReplyInvoke([this, lifetimeToken, probe, normalized](
                             std::optional<sdbus::Error> err, std::map<std::string, sdbus::Variant> properties
                         ) {
          if (lifetimeToken.expired() || err.has_value() || m_activeDevicePath != normalized) {
            return;
          }
          std::string apPath;
          if (auto it = properties.find("ActiveAccessPoint"); it != properties.end()) {
            try {
              apPath = it->second.get<sdbus::ObjectPath>();
            } catch (const sdbus::Error&) {
            }
          }
          rebindActiveAccessPoint(apPath);
          refresh();
        });
  } catch (const sdbus::Error& e) {
    kLog.debug("device wireless probe failed {}: {}", normalized, e.what());
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
  next->vpnConnected = m_anyVpnConnected;

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
    // vpnActive is informational only — an active VPN must not masquerade as
    // physical connectivity (connected/kind describe the physical link).
    if (!next->vpnActive && vpnFromList) {
      next->vpnActive = true;
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
              } else if (
                  deviceType == kNmDeviceTypeEthernet
                  || deviceType == kNmDeviceTypeBridge
                  || deviceType == kNmDeviceTypeBond
                  || deviceType == kNmDeviceTypeTeam
                  || deviceType == kNmDeviceTypeVlan
              ) {
                next->kind = NetworkConnectivity::Wired;
              }
              // Remaining device types (wireguard, tun, …) are VPN/overlay virtual
              // links and must not be reported as wired; kind stays Unknown.

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
              next->resolving = state == kNmActiveConnectionStateActivating;
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
