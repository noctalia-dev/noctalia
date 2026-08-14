#pragma once

#include "dbus/network/inetwork_service.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class SystemBus;

namespace sdbus {
  class IProxy;
}

// Network backend for systems running wpa_supplicant without NetworkManager.
// Exposes the same INetworkService interface as NetworkManagerService.
class WpaSupplicantService : public INetworkService {
public:
  explicit WpaSupplicantService(SystemBus& bus);
  ~WpaSupplicantService() override;

  WpaSupplicantService(const WpaSupplicantService&) = delete;
  WpaSupplicantService& operator=(const WpaSupplicantService&) = delete;

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

private:
  void subscribeInterface(const std::string& ifacePath);
  void scheduleRebuild();
  void rebuildState();
  void emitChangedIfNeeded(NetworkState next);
  void loadSavedNetworks(const std::string& ifacePath, sdbus::IProxy& proxy);
  sdbus::IProxy* firstInterface() const;

  SystemBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_wpa;
  std::unordered_map<std::string, std::unique_ptr<sdbus::IProxy>> m_interfaces;
  // BSS path -> cached proxy, populated via BSSAdded/BSSRemoved signals.
  std::unordered_map<std::string, std::unique_ptr<sdbus::IProxy>> m_bssProxies;
  // ssid -> network object path.
  std::unordered_map<std::string, std::string> m_savedNetworks;
  NetworkState m_state;
  std::vector<AccessPointInfo> m_accessPoints;
  const std::vector<VpnConnectionInfo> m_vpnConnections; // always empty
  bool m_hasStateSnapshot = false;
  bool m_rebuildPending = false;
  std::optional<bool> m_wirelessEnabledOverride;
  ChangeCallback m_changeCallback;
};
