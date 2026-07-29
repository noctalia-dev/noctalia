#pragma once

#include "dbus/network/inetwork_service.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
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

  // Activate / deactivate a saved VPN connection profile. Deactivate also
  // aborts a connection that is stuck activating.
  bool activateVpnConnection(const VpnConnectionInfo& vpn) override;
  bool deactivateVpnConnection(const VpnConnectionInfo& vpn) override;
  [[nodiscard]] bool canActivateWiredConnection() const noexcept override;
  bool activateWiredConnection() override;

  // Enable / disable the Wi-Fi radio.
  void setWirelessEnabled(bool enabled) override;

  // Disconnect the active physical connection.
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
  void reconcileVpnActiveWatchers(const std::set<std::string>& activePaths);
  void finishSavedConnections(
      std::vector<std::string>& ssids, std::vector<std::string>& wiredConnectionPaths, std::function<void()> onComplete
  );
  void finishRefreshAccessPoints(std::vector<AccessPointInfo>& aps, std::function<void()> onComplete);
  bool addAndActivateAccessPoint(const AccessPointInfo& ap, const std::optional<std::string>& psk);
  void watchPendingAccessPointActivation(
      const std::string& ssid, const std::string& connectionPath, const std::string& activePath
  );
  void handlePendingAccessPointActivationState(const std::string& activePath, std::uint32_t state);
  void persistConnectionToDisk(const std::string& connectionPath, const std::string& ssid);
  void deleteUnsavedConnection(const std::string& connectionPath, const std::string& ssid);
  // Async rebind pipeline. All proxy destruction happens in async reply
  // context, never inside a proxy's own signal handler.
  void requestRebind();
  // allowActivatedAsPrimary: when false (no NM PrimaryConnection yet), a fully
  // activated physical device must not be reported as the connected primary — it
  // may be a bridge/bond slave or a link that has not become the default route.
  // Only mid-activation links are surfaced (resolving state).
  void resolvePhysicalPrimary(
      bool allowActivatedAsPrimary, std::function<void(std::string connectionPath, std::string devicePath)> done
  );
  void adoptActiveConnection(const std::string& connectionPath, const std::string& devicePath);
  void rebindActiveDevice(const std::string& devicePath);
  void rebindActiveAccessPoint(const std::string& apPath);
  void ensureWifiDeviceSubscribed(const std::string& devicePath);
  void
  collectWifiDevices(std::function<void(std::vector<std::string> devicePaths, std::int64_t lastScanBaseline)> done);
  void tryActivateWiredConnection(std::shared_ptr<std::vector<std::string>> candidates, std::size_t index);
  void readStateAsync(std::function<void(NetworkState)> onComplete);
  [[nodiscard]] NetworkChangeOrigin consumeWirelessEnabledChangeOrigin(bool enabled);

  struct PendingAccessPointActivation;

  SystemBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_nm;
  std::unique_ptr<sdbus::IProxy> m_activeConnection;
  std::unique_ptr<sdbus::IProxy> m_activeDevice;
  std::unique_ptr<sdbus::IProxy> m_activeAp;
  std::unordered_map<std::string, std::unique_ptr<sdbus::IProxy>> m_wifiDevices;
  std::unordered_map<std::string, std::unique_ptr<sdbus::IProxy>> m_vpnActiveWatchers;
  std::string m_activeConnectionPath;
  std::string m_activeDevicePath;
  std::string m_activeApPath;
  NetworkState m_state;
  std::vector<AccessPointInfo> m_accessPoints;
  std::vector<VpnConnectionInfo> m_vpnConnections;
  std::vector<std::string> m_savedSsids;
  std::vector<std::string> m_savedWiredConnectionPaths;
  std::unordered_map<std::string, std::unique_ptr<PendingAccessPointActivation>> m_pendingApActivations;
  // Finished activations whose proxy may still be executing its own handler;
  // freed at the next refresh completion (an async reply context).
  std::vector<std::unique_ptr<PendingAccessPointActivation>> m_retiredApActivations;
  std::shared_ptr<int> m_lifetimeToken;
  bool m_refreshInFlight = false;
  bool m_refreshQueued = false;
  bool m_rebindInFlight = false;
  bool m_rebindQueued = false;
  bool m_emitOnNextRefresh = false;
  bool m_scanning = false;
  bool m_anyVpnConnected = false;
  std::int64_t m_scanBaselineLastScan = 0;
  std::optional<bool> m_pendingLocalWirelessEnabled;
  bool m_hasStateSnapshot = false;
  ChangeCallback m_changeCallback;
};
