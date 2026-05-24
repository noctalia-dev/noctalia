#pragma once

#include "dbus/network/inetwork_service.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class SystemBus;

namespace sdbus {
  class IProxy;
}

class NetworkManagerService : public INetworkService {
public:
  using ChangeCallback = std::function<void(const NetworkState&, NetworkChangeOrigin)>;

  explicit NetworkManagerService(SystemBus& bus);
  ~NetworkManagerService() override;

  NetworkManagerService(const NetworkManagerService&) = delete;
  NetworkManagerService& operator=(const NetworkManagerService&) = delete;

  void setChangeCallback(ChangeCallback callback) override;
  void refresh() override;

  [[nodiscard]] const NetworkState& state() const noexcept override { return m_state; }
  [[nodiscard]] bool hasStateSnapshot() const noexcept override { return m_hasStateSnapshot; }
  [[nodiscard]] const std::vector<AccessPointInfo>& accessPoints() const noexcept override { return m_accessPoints; }
  [[nodiscard]] const std::vector<VpnConnectionInfo>& vpnConnections() const noexcept override {
    return m_vpnConnections;
  }

  // Trigger a Wi-Fi scan on every wifi device. Results arrive via PropertiesChanged.
  void requestScan() override;

  // Activate a saved connection for the given access point, or create an
  // in-memory profile for a new network and persist it after activation succeeds.
  // NM picks the matching saved connection automatically when the first argument is "/".
  // Returns false only on an immediate D-Bus error.
  bool activateAccessPoint(const AccessPointInfo& ap) override;
  bool activateAccessPoint(const AccessPointInfo& ap, const std::string& psk) override;

  // Activate / deactivate a saved VPN connection profile.
  bool activateVpnConnection(const VpnConnectionInfo& vpn) override;
  bool deactivateVpnConnection(const VpnConnectionInfo& vpn) override;

  // Enable / disable the Wi-Fi radio.
  void setWirelessEnabled(bool enabled) override;

  // Deactivate the current primary connection.
  void disconnect() override;

  // Delete every saved connection whose 802-11-wireless SSID matches.
  void forgetSsid(const std::string& ssid) override;

  // Whether any saved connection matches the SSID (uses cached snapshot refreshed on every refresh()).
  [[nodiscard]] bool hasSavedConnection(const std::string& ssid) const override;
  [[nodiscard]] bool supportsSecretAgent() const noexcept override { return true; }

private:
  void refreshAccessPoints(std::function<void()> onComplete);
  void refreshSavedConnections(std::function<void()> onComplete);
  void refreshVpnConnections(std::function<void()> onComplete);
  void finishSavedConnections(std::vector<std::string>& ssids, std::function<void()> onComplete);
  void finishRefreshAccessPoints(std::vector<AccessPointInfo>& aps, std::function<void()> onComplete);
  bool addAndActivateAccessPoint(const AccessPointInfo& ap, const std::optional<std::string>& psk);
  void watchPendingAccessPointActivation(
      const std::string& ssid, const std::string& connectionPath, const std::string& activePath
  );
  void handlePendingAccessPointActivationState(const std::string& activePath, std::uint32_t state);
  void persistConnectionToDisk(const std::string& connectionPath, const std::string& ssid);
  void deleteUnsavedConnection(const std::string& connectionPath, const std::string& ssid);
  void rebindActiveConnection();
  void rebindActiveDevice(const std::string& devicePath);
  void rebindActiveAccessPoint(const std::string& apPath);
  void ensureWifiDeviceSubscribed(const std::string& devicePath);
  void readStateAsync(std::function<void(NetworkState)> onComplete);
  [[nodiscard]] NetworkChangeOrigin consumeWirelessEnabledChangeOrigin(bool enabled);
  void emitChangedIfNeeded(NetworkState next);

  struct PendingAccessPointActivation;

  SystemBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_nm;
  std::unique_ptr<sdbus::IProxy> m_activeConnection;
  std::unique_ptr<sdbus::IProxy> m_activeDevice;
  std::unique_ptr<sdbus::IProxy> m_activeAp;
  std::unordered_map<std::string, std::unique_ptr<sdbus::IProxy>> m_wifiDevices;
  std::string m_activeConnectionPath;
  std::string m_activeDevicePath;
  std::string m_activeApPath;
  NetworkState m_state;
  std::vector<AccessPointInfo> m_accessPoints;
  std::vector<VpnConnectionInfo> m_vpnConnections;
  std::vector<std::string> m_savedSsids;
  std::unordered_map<std::string, std::unique_ptr<PendingAccessPointActivation>> m_pendingApActivations;
  std::shared_ptr<int> m_lifetimeToken;
  bool m_refreshInFlight = false;
  bool m_refreshQueued = false;
  bool m_scanning = false;
  std::int64_t m_scanBaselineLastScan = 0;
  std::optional<bool> m_pendingLocalWirelessEnabled;
  bool m_hasStateSnapshot = false;
  ChangeCallback m_changeCallback;
};
