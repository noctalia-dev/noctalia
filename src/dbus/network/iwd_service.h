#pragma once

#include "dbus/network/inetwork_service.h"

#include <map>
#include <memory>
#include <sdbus-c++/Types.h>
#include <string>
#include <unordered_map>
#include <vector>

class SystemBus;

namespace sdbus {
  class IProxy;
}

// Network backend for systems running iwd without NetworkManager.
class IwdService : public INetworkService {
public:
  explicit IwdService(SystemBus& bus);
  ~IwdService() override;

  IwdService(const IwdService&) = delete;
  IwdService& operator=(const IwdService&) = delete;

  void setChangeCallback(ChangeCallback callback) override;
  void refresh() override;

  [[nodiscard]] const NetworkState& state() const noexcept override { return m_state; }
  [[nodiscard]] bool hasStateSnapshot() const noexcept override { return m_hasStateSnapshot; }
  [[nodiscard]] const std::vector<AccessPointInfo>& accessPoints() const noexcept override { return m_accessPoints; }
  [[nodiscard]] const std::vector<VpnConnectionInfo>& vpnConnections() const noexcept override {
    return m_vpnConnections;
  }

  void requestScan() override;
  bool activateAccessPoint(const AccessPointInfo& ap) override;
  bool activateAccessPoint(const AccessPointInfo& ap, const std::string& psk) override;
  bool activateVpnConnection(const VpnConnectionInfo& /*vpn*/) override { return false; }
  bool deactivateVpnConnection(const VpnConnectionInfo& /*vpn*/) override { return false; }
  void setWirelessEnabled(bool enabled, WirelessEnabledCompletion onComplete = {}) override;
  void disconnect() override;
  void forgetSsid(const std::string& ssid) override;
  [[nodiscard]] bool hasSavedConnection(const std::string& ssid) const override;

  void setSecretAgent(class IwdSecretAgent* agent);

private:
  void subscribeObject(
      const std::string& path, const std::map<std::string, std::map<std::string, sdbus::Variant>>& interfaces
  );
  void emitChangedIfNeeded(NetworkState next);

  SystemBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_iwd;
  NetworkState m_state;
  std::vector<AccessPointInfo> m_accessPoints;
  const std::vector<VpnConnectionInfo> m_vpnConnections; // always empty
  // ssid -> known network object path.
  std::unordered_map<std::string, std::string> m_knownNetworks;
  // station path -> device name.
  std::unordered_map<std::string, std::string> m_deviceNames;
  std::unordered_map<std::string, std::unique_ptr<sdbus::IProxy>> m_objectProxies;
  std::unique_ptr<sdbus::IProxy> m_connectProxy; // keeps an in-flight async Connect alive
  IwdSecretAgent* m_secretAgent = nullptr;
  std::string m_pendingPsk;
  bool m_hasStateSnapshot = false;
  bool m_refreshInFlight = false;
  bool m_refreshQueued = false;
  ChangeCallback m_changeCallback;
};
