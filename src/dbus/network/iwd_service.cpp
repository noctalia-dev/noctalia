#include "dbus/network/iwd_service.h"

#include "core/log.h"
#include "dbus/network/iwd_diagnostics.h"
#include "dbus/network/iwd_secret_agent.h"
#include "dbus/system_bus.h"
#include "system/rfkill_helper.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("iwd");

  const sdbus::ServiceName kIwdBusName{"net.connman.iwd"};
  const sdbus::ObjectPath kRootPath{"/"};
  constexpr auto kDeviceInterface = "net.connman.iwd.Device";
  constexpr auto kStationInterface = "net.connman.iwd.Station";
  constexpr auto kStationDiagnosticInterface = "net.connman.iwd.StationDiagnostic";
  constexpr auto kNetworkInterface = "net.connman.iwd.Network";
  constexpr auto kKnownNetworkInterface = "net.connman.iwd.KnownNetwork";
  constexpr auto kObjectManagerInterface = "org.freedesktop.DBus.ObjectManager";

  using VariantMap = std::map<std::string, sdbus::Variant>;
  using ObjectInterfaces = std::map<std::string, VariantMap>;
  using ManagedObjects = std::map<sdbus::ObjectPath, ObjectInterfaces>;
  using OrderedNetwork = sdbus::Struct<sdbus::ObjectPath, std::int16_t>;

  struct StationRefreshState {
    ManagedObjects objects;
    NetworkState next;
    std::vector<AccessPointInfo> aps;
    int pending = 0;
  };

  template <typename T> std::optional<T> variantGet(const sdbus::Variant& value) {
    try {
      return value.get<T>();
    } catch (const sdbus::Error&) {
      return std::nullopt;
    }
  }

  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";

  std::uint8_t signalFromIwdStrength(std::int16_t centiDbm) { return iwd_diagnostics::signalToPercent(centiDbm / 100); }

  std::optional<std::string> stringProp(const VariantMap& props, std::string_view name) {
    const auto it = props.find(std::string{name});
    if (it == props.end()) {
      return std::nullopt;
    }
    return variantGet<std::string>(it->second);
  }

  std::optional<bool> boolProp(const VariantMap& props, std::string_view name) {
    const auto it = props.find(std::string{name});
    if (it == props.end()) {
      return std::nullopt;
    }
    return variantGet<bool>(it->second);
  }

  std::optional<sdbus::ObjectPath> objectPathProp(const VariantMap& props, std::string_view name) {
    const auto it = props.find(std::string{name});
    if (it == props.end()) {
      return std::nullopt;
    }
    return variantGet<sdbus::ObjectPath>(it->second);
  }

  bool isRootPath(const sdbus::ObjectPath& path) { return std::string(path) == "/"; }

} // namespace

IwdService::IwdService(SystemBus& bus) : m_bus(bus) {
  m_lifetimeToken = std::make_shared<int>(0);
  if (!bus.nameHasOwner("net.connman.iwd")) {
    throw sdbus::Error(
        sdbus::Error::Name{"org.freedesktop.DBus.Error.ServiceUnknown"},
        "The name net.connman.iwd was not provided by any .service files"
    );
  }

  m_iwd = sdbus::createProxy(m_bus.connection(), kIwdBusName, kRootPath);
  const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
  m_iwd->uponSignal("InterfacesAdded")
      .onInterface(kObjectManagerInterface)
      .call([this, lifetimeToken](const sdbus::ObjectPath&, const ObjectInterfaces&) {
        if (lifetimeToken.expired()) {
          return;
        }
        refresh();
      });
  m_iwd->uponSignal("InterfacesRemoved")
      .onInterface(kObjectManagerInterface)
      .call([this, lifetimeToken](const sdbus::ObjectPath&, const std::vector<std::string>&) {
        if (lifetimeToken.expired()) {
          return;
        }
        refresh();
      });

  refresh();
}

IwdService::~IwdService() { m_lifetimeToken.reset(); }

void IwdService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void IwdService::setSecretAgent(IwdSecretAgent* agent) {
  m_secretAgent = agent;
  if (m_secretAgent != nullptr) {
    const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
    m_secretAgent->setRequestCallback([this, lifetimeToken](const IwdSecretAgent::SecretRequest& request) {
      (void)request;
      if (lifetimeToken.expired()) {
        return;
      }
      if (!m_pendingPsk.empty()) {
        m_secretAgent->submitSecret(m_pendingPsk);
        m_pendingPsk.clear();
      } else {
        m_secretAgent->cancelSecret();
      }
    });
  }
}

void IwdService::subscribeObject(const std::string& path, const ObjectInterfaces& interfaces) {
  if (m_objectProxies.contains(path)) {
    return;
  }
  if (!interfaces.contains(kDeviceInterface)
      && !interfaces.contains(kStationInterface)
      && !interfaces.contains(kNetworkInterface)
      && !interfaces.contains(kKnownNetworkInterface)) {
    return;
  }

  try {
    auto proxy = sdbus::createProxy(m_bus.connection(), kIwdBusName, sdbus::ObjectPath{path});
    const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
    proxy->uponSignal("PropertiesChanged")
        .onInterface(kPropertiesInterface)
        .call([this,
               lifetimeToken](const std::string& interfaceName, const VariantMap&, const std::vector<std::string>&) {
          if (lifetimeToken.expired()) {
            return;
          }
          if (interfaceName == kDeviceInterface
              || interfaceName == kStationInterface
              || interfaceName == kNetworkInterface
              || interfaceName == kKnownNetworkInterface) {
            refresh();
          }
        });
    m_objectProxies.emplace(path, std::move(proxy));
  } catch (const sdbus::Error& e) {
    kLog.debug("failed to subscribe IWD object {}: {}", path, e.what());
  }
}

void IwdService::refresh() {
  const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
  if (m_refreshInFlight) {
    m_refreshQueued = true;
    return;
  }
  m_refreshInFlight = true;

  auto finish = [this] {
    m_refreshInFlight = false;
    if (m_refreshQueued) {
      m_refreshQueued = false;
      refresh();
    }
  };

  auto publish = [this, finish](NetworkState next, std::vector<AccessPointInfo> aps) {
    std::ranges::sort(aps, [](const AccessPointInfo& a, const AccessPointInfo& b) {
      if (a.active != b.active) {
        return a.active > b.active;
      }
      return a.strength > b.strength;
    });
    m_accessPoints = std::move(aps);
    emitChangedIfNeeded(std::move(next));
    finish();
  };

  auto readStations = [this, lifetimeToken, publish](ManagedObjects objects) {
    NetworkState next;
    std::vector<std::string> liveObjects;
    m_knownNetworks.clear();
    m_deviceNames.clear();

    for (const auto& [path, interfaces] : objects) {
      const std::string pathString = std::string(path);
      subscribeObject(pathString, interfaces);
      liveObjects.push_back(pathString);

      if (const auto it = interfaces.find(kDeviceInterface); it != interfaces.end()) {
        const auto name = stringProp(it->second, "Name").value_or("");
        const auto powered = boolProp(it->second, "Powered").value_or(true);
        next.wirelessEnabled = next.wirelessEnabled || powered;
        if (!name.empty()) {
          m_deviceNames[pathString] = name;
        }
      }
      if (const auto it = interfaces.find(kKnownNetworkInterface); it != interfaces.end()) {
        if (auto name = stringProp(it->second, "Name"); name.has_value() && !name->empty()) {
          m_knownNetworks[*name] = pathString;
        }
      }
    }

    for (auto it = m_objectProxies.begin(); it != m_objectProxies.end();) {
      if (std::ranges::find(liveObjects, it->first) == liveObjects.end()) {
        it = m_objectProxies.erase(it);
      } else {
        ++it;
      }
    }

    std::vector<std::pair<sdbus::ObjectPath, ObjectInterfaces>> stations;
    for (const auto& [path, interfaces] : objects) {
      if (interfaces.contains(kStationInterface)) {
        stations.emplace_back(path, interfaces);
      }
    }
    if (stations.empty()) {
      publish(std::move(next), {});
      return;
    }

    auto state = std::make_shared<StationRefreshState>();
    state->objects = std::move(objects);
    state->next = std::move(next);
    state->pending = static_cast<int>(stations.size());

    for (const auto& [stationPath, interfaces] : stations) {
      const auto stationIt = interfaces.find(kStationInterface);
      std::shared_ptr<sdbus::IProxy> stationProxy;
      try {
        stationProxy = std::shared_ptr<sdbus::IProxy>(sdbus::createProxy(m_bus.connection(), kIwdBusName, stationPath));
      } catch (const sdbus::Error& e) {
        kLog.debug("station proxy failed on {}: {}", std::string(stationPath), e.what());
        if (--state->pending == 0) {
          publish(std::move(state->next), std::move(state->aps));
        }
        continue;
      }

      const auto stateValue = stringProp(stationIt->second, "State").value_or("");
      const bool connected = stateValue == "connected";
      const bool connecting =
          stateValue == "connecting" || stateValue == "associating" || stateValue == "authenticating";
      state->next.scanning = state->next.scanning || boolProp(stationIt->second, "Scanning").value_or(false);

      std::string connectedNetworkPath;
      if (auto connectedNetwork = objectPathProp(stationIt->second, "ConnectedNetwork");
          connectedNetwork.has_value() && !isRootPath(*connectedNetwork)) {
        connectedNetworkPath = std::string(*connectedNetwork);
      }

      if (connected || connecting) {
        state->next.kind = NetworkConnectivity::Wireless;
        state->next.connected = connected;
        state->next.resolving = connecting;
        state->next.interfaceName = m_deviceNames[std::string(stationPath)];
      }

      auto finishStation = [lifetimeToken, state, stationProxy, connected, publish] {
        if (!connected) {
          if (--state->pending == 0) {
            publish(std::move(state->next), std::move(state->aps));
          }
          return;
        }
        try {
          stationProxy->callMethodAsync("GetDiagnostics")
              .onInterface(kStationDiagnosticInterface)
              .uponReplyInvoke(
                  [lifetimeToken, stationProxy, state, publish](
                      std::optional<sdbus::Error> diagnosticsErr, // NOLINT(performance-unnecessary-value-param)
                      const VariantMap& diagnostics
                  ) {
                    if (lifetimeToken.expired()) {
                      return;
                    }
                    if (!diagnosticsErr.has_value()) {
                      iwd_diagnostics::applyStationDiagnostics(state->next, diagnostics);
                    }
                    if (--state->pending == 0) {
                      publish(std::move(state->next), std::move(state->aps));
                    }
                  }
              );
        } catch (const sdbus::Error& e) {
          kLog.debug("GetDiagnostics dispatch failed: {}", e.what());
          if (--state->pending == 0) {
            publish(std::move(state->next), std::move(state->aps));
          }
        }
      };

      try {
        stationProxy->callMethodAsync("GetOrderedNetworks")
            .onInterface(kStationInterface)
            .uponReplyInvoke([lifetimeToken, state, stationPath, connectedNetworkPath, finishStation](
                                 std::optional<sdbus::Error> orderedErr, // NOLINT(performance-unnecessary-value-param)
                                 const std::vector<OrderedNetwork>& orderedNetworks
                             ) {
              if (lifetimeToken.expired()) {
                return;
              }
              if (!orderedErr.has_value()) {
                for (const auto& [networkPath, signal] : orderedNetworks) {
                  const auto objectIt = state->objects.find(networkPath);
                  if (objectIt == state->objects.end()) {
                    continue;
                  }
                  const auto networkIt = objectIt->second.find(kNetworkInterface);
                  if (networkIt == objectIt->second.end()) {
                    continue;
                  }

                  const auto ssid = stringProp(networkIt->second, "Name").value_or("");
                  if (ssid.empty()) {
                    continue;
                  }
                  const auto type = stringProp(networkIt->second, "Type").value_or("");
                  const bool active = boolProp(networkIt->second, "Connected").value_or(false)
                      || (!connectedNetworkPath.empty() && connectedNetworkPath == std::string(networkPath));
                  const std::uint8_t strength = signalFromIwdStrength(signal);

                  auto existing = std::ranges::find(state->aps, ssid, &AccessPointInfo::ssid);
                  if (existing != state->aps.end()) {
                    existing->strength = std::max(existing->strength, strength);
                    existing->active = existing->active || active;
                    existing->secured = existing->secured || type != "open";
                  } else {
                    AccessPointInfo ap;
                    ap.path = std::string(networkPath);
                    ap.devicePath = std::string(stationPath);
                    ap.ssid = ssid;
                    ap.strength = strength;
                    ap.secured = type != "open";
                    ap.active = active;
                    state->aps.push_back(std::move(ap));
                  }

                  if (active) {
                    state->next.ssid = ssid;
                    state->next.signalStrength = std::max(state->next.signalStrength, strength);
                  }
                }
              } else {
                kLog.debug("GetOrderedNetworks failed on {}: {}", std::string(stationPath), orderedErr->what());
              }
              finishStation();
            });
      } catch (const sdbus::Error& e) {
        kLog.debug("GetOrderedNetworks dispatch failed on {}: {}", std::string(stationPath), e.what());
        finishStation();
      }
    }
  };

  try {
    m_iwd->callMethodAsync("GetManagedObjects")
        .onInterface(kObjectManagerInterface)
        .uponReplyInvoke([lifetimeToken, readStations, finish](
                             std::optional<sdbus::Error> error, // NOLINT(performance-unnecessary-value-param)
                             ManagedObjects objects
                         ) {
          if (lifetimeToken.expired()) {
            return;
          }
          if (error.has_value()) {
            kLog.warn("GetManagedObjects failed: {}", error->what());
            finish();
            return;
          }
          readStations(std::move(objects));
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("GetManagedObjects dispatch failed: {}", e.what());
    finish();
  }
}

void IwdService::requestScan() {
  const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
  const std::uint64_t generation = ++m_scanGeneration;
  std::vector<std::string> stationPaths;
  stationPaths.reserve(m_deviceNames.size());
  for (const auto& [stationPath, ifname] : m_deviceNames) {
    (void)ifname;
    stationPaths.push_back(stationPath);
  }
  if (stationPaths.empty()) {
    return;
  }

  auto pending = std::make_shared<int>(static_cast<int>(stationPaths.size()));
  auto scanFinished = [this, pending] {
    if (--*pending == 0) {
      refresh();
    }
  };

  for (const auto& stationPath : stationPaths) {
    try {
      auto station = std::shared_ptr<sdbus::IProxy>(
          sdbus::createProxy(m_bus.connection(), kIwdBusName, sdbus::ObjectPath{stationPath})
      );
      station->callMethodAsync("Scan")
          .onInterface(kStationInterface)
          .uponReplyInvoke([this, lifetimeToken, generation, stationPath, station, scanFinished](
                               std::optional<sdbus::Error> error // NOLINT(performance-unnecessary-value-param)
                           ) {
            if (lifetimeToken.expired() || generation != m_scanGeneration) {
              return;
            }
            if (error.has_value()) {
              kLog.debug("Scan failed on {}: {}", stationPath, error->what());
            }
            scanFinished();
          });
    } catch (const sdbus::Error& e) {
      kLog.debug("Scan dispatch failed on {}: {}", stationPath, e.what());
      scanFinished();
    }
  }
}

bool IwdService::activateAccessPoint(const AccessPointInfo& ap) {
  const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
  try {
    // Starting a new connect supersedes any previous in-flight one.
    m_connectProxy =
        std::shared_ptr<sdbus::IProxy>(sdbus::createProxy(m_bus.connection(), kIwdBusName, sdbus::ObjectPath{ap.path}));
    auto connectProxy = m_connectProxy;
    connectProxy->callMethodAsync("Connect")
        .onInterface(kNetworkInterface)
        .uponReplyInvoke([this, lifetimeToken, connectProxy, ssid = ap.ssid](
                             std::optional<sdbus::Error> error // NOLINT(performance-unnecessary-value-param)
                         ) {
          if (lifetimeToken.expired()) {
            return;
          }
          if (m_connectProxy == connectProxy) {
            // consumed by the agent, or abandoned, never keep it around
            m_pendingPsk.clear();
          }
          if (error.has_value()) {
            kLog.warn("Connect failed for {}: {}", ssid, error->what());
          }
          refresh();
        });
    // dispatched; real result arrives via the change callback
    return true;
  } catch (const sdbus::Error& e) {
    m_pendingPsk.clear();
    kLog.warn("Connect dispatch failed for {}: {}", ap.ssid, e.what());
    return false;
  }
}

bool IwdService::activateAccessPoint(const AccessPointInfo& ap, const std::string& psk) {
  if (psk.empty()) {
    return activateAccessPoint(ap);
  }
  if (m_secretAgent == nullptr) {
    kLog.warn("IWD secret agent not available; cannot connect to secured network");
    return false;
  }
  // Store the PSK for the agent to submit when RequestPassphrase is called
  m_pendingPsk = psk;
  return activateAccessPoint(ap);
}

void IwdService::setWirelessEnabled(bool enabled, WirelessEnabledCompletion onComplete) {
  const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
  std::vector<std::string> devicePaths;
  devicePaths.reserve(m_deviceNames.size());
  for (const auto& [stationPath, ifname] : m_deviceNames) {
    (void)ifname;
    devicePaths.push_back(stationPath);
  }

  const bool softBlocked = !enabled;
  bool rfkillDone = false;
  for (const auto& [stationPath, ifname] : m_deviceNames) {
    (void)stationPath;
    if (ifname.empty()) {
      continue;
    }
    const RfkillSwitchResult result = setRfkillSoftBlockedForNetInterface(ifname, softBlocked);
    if (result.hardBlocked) {
      kLog.warn("setWirelessEnabled: rfkill reports a hard block on {}", ifname);
    }
    if (result.success) {
      rfkillDone = true;
      break;
    }
  }

  if (!rfkillDone) {
    const RfkillSwitchResult fallback = setRfkillSoftBlocked(RfkillDeviceType::Wlan, softBlocked);
    if (fallback.hardBlocked) {
      kLog.warn("setWirelessEnabled: wlan rfkill reports a hard block");
    }
    if (fallback.success) {
      rfkillDone = true;
    } else if (!fallback.detail.empty()) {
      kLog.debug("setWirelessEnabled: wlan rfkill fallback: {}", fallback.detail);
    }
  }

  auto complete = [this, lifetimeToken, onComplete](bool success) {
    if (lifetimeToken.expired()) {
      return;
    }
    refresh();
    if (onComplete) {
      onComplete(success);
    }
  };

  if (devicePaths.empty()) {
    complete(rfkillDone);
    return;
  }

  auto pending = std::make_shared<int>(static_cast<int>(devicePaths.size()));
  auto anyPowered = std::make_shared<bool>(false);
  auto devicesDone = [lifetimeToken, complete, pending, anyPowered] {
    if (lifetimeToken.expired()) {
      return;
    }
    if (--*pending == 0) {
      complete(*anyPowered);
    }
  };

  for (const auto& devicePath : devicePaths) {
    try {
      auto device = std::shared_ptr<sdbus::IProxy>(
          sdbus::createProxy(m_bus.connection(), kIwdBusName, sdbus::ObjectPath{devicePath})
      );
      device->setPropertyAsync("Powered")
          .onInterface(kDeviceInterface)
          .toValue(enabled)
          .uponReplyInvoke([lifetimeToken, device, devicePath, pending, anyPowered, devicesDone](
                               std::optional<sdbus::Error> error // NOLINT(performance-unnecessary-value-param)
                           ) {
            if (lifetimeToken.expired()) {
              return;
            }
            if (error.has_value()) {
              kLog.debug("setting IWD Powered failed on {}: {}", devicePath, error->what());
            } else {
              *anyPowered = true;
            }
            devicesDone();
          });
    } catch (const sdbus::Error& e) {
      kLog.debug("setting IWD Powered dispatch failed on {}: {}", devicePath, e.what());
      devicesDone();
    }
  }
}

void IwdService::disconnect() {
  const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
  std::vector<std::string> stationPaths;
  stationPaths.reserve(m_deviceNames.size());
  for (const auto& [stationPath, ifname] : m_deviceNames) {
    (void)ifname;
    stationPaths.push_back(stationPath);
  }
  if (stationPaths.empty()) {
    refresh();
    return;
  }

  auto pending = std::make_shared<int>(static_cast<int>(stationPaths.size()));
  auto stationFinished = [this, pending] {
    if (--*pending == 0) {
      refresh();
    }
  };

  for (const auto& stationPath : stationPaths) {
    try {
      auto station = std::shared_ptr<sdbus::IProxy>(
          sdbus::createProxy(m_bus.connection(), kIwdBusName, sdbus::ObjectPath{stationPath})
      );
      station->callMethodAsync("Disconnect")
          .onInterface(kStationInterface)
          .uponReplyInvoke([lifetimeToken, stationPath, station, stationFinished](
                               std::optional<sdbus::Error> error // NOLINT(performance-unnecessary-value-param)
                           ) {
            if (lifetimeToken.expired()) {
              return;
            }
            if (error.has_value()) {
              kLog.debug("Disconnect failed on {}: {}", stationPath, error->what());
            }
            stationFinished();
          });
    } catch (const sdbus::Error& e) {
      kLog.debug("Disconnect dispatch failed on {}: {}", stationPath, e.what());
      stationFinished();
    }
  }
}

void IwdService::forgetSsid(const std::string& ssid) {
  const auto it = m_knownNetworks.find(ssid);
  if (it == m_knownNetworks.end()) {
    return;
  }
  const std::string networkPath = it->second;
  const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
  try {
    auto knownNetwork = std::shared_ptr<sdbus::IProxy>(
        sdbus::createProxy(m_bus.connection(), kIwdBusName, sdbus::ObjectPath{networkPath})
    );
    knownNetwork->callMethodAsync("Forget")
        .onInterface(kKnownNetworkInterface)
        .uponReplyInvoke([this, lifetimeToken, ssid, networkPath, knownNetwork](
                             std::optional<sdbus::Error> error // NOLINT(performance-unnecessary-value-param)
                         ) {
          if (lifetimeToken.expired()) {
            return;
          }
          if (error.has_value()) {
            kLog.warn("Forget failed for {}: {}", ssid, error->what());
            return;
          }
          const auto current = m_knownNetworks.find(ssid);
          if (current != m_knownNetworks.end() && current->second == networkPath) {
            m_knownNetworks.erase(current);
          }
          refresh();
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("Forget dispatch failed for {}: {}", ssid, e.what());
  }
}

bool IwdService::hasSavedConnection(const std::string& ssid) const { return m_knownNetworks.contains(ssid); }

void IwdService::emitChangedIfNeeded(NetworkState next) {
  const bool firstSnapshot = !m_hasStateSnapshot;
  const bool stateChanged = next != m_state;
  m_state = std::move(next);
  m_hasStateSnapshot = true;
  if ((firstSnapshot || stateChanged) && m_changeCallback) {
    m_changeCallback(m_state, NetworkChangeOrigin::External);
  }
}
