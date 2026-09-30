#pragma once

#include "dbus/network/inetwork_service.h"

#include <memory>
#include <string>

class NetworkService final : public INetworkService {
public:
  NetworkService(std::unique_ptr<INetworkService> networkManager, std::unique_ptr<INetworkService> fallback);
  void setChangeCallback(ChangeCallback callback) override;
  void refresh() override;

  [[nodiscard]] bool available() const noexcept override;
  [[nodiscard]] const NetworkState& state() const noexcept override;
  [[nodiscard]] bool hasStateSnapshot() const noexcept override;
  [[nodiscard]] const std::vector<AccessPointInfo>& accessPoints() const noexcept override;
  [[nodiscard]] const std::vector<VpnConnectionInfo>& vpnConnections() const noexcept override;

  void requestScan() override;
  bool activateAccessPoint(const AccessPointInfo& ap) override;
  bool activateAccessPoint(const AccessPointInfo& ap, const std::string& psk) override;
  [[nodiscard]] bool supportsEnterprise() const noexcept override;
  bool activateEnterpriseAccessPoint(
      const AccessPointInfo& ap, const network_enterprise::EnterpriseCredentials& credentials
  ) override;
  bool activateVpnConnection(const VpnConnectionInfo& vpn) override;
  bool deactivateVpnConnection(const VpnConnectionInfo& vpn) override;
  [[nodiscard]] bool canActivateWiredConnection() const noexcept override;
  bool activateWiredConnection() override;
  [[nodiscard]] bool canActivateCellularConnection() const noexcept override;
  bool activateCellularConnection() override;
  bool deactivateCellularConnection() override;
  void setWirelessEnabled(bool enabled, WirelessEnabledCompletion onComplete = {}) override;
  void disconnect() override;
  void forgetSsid(const std::string& ssid) override;
  [[nodiscard]] bool hasSavedConnection(const std::string& ssid) const override;

private:
  [[nodiscard]] INetworkService& active() noexcept;
  [[nodiscard]] const INetworkService& active() const noexcept;
  void watchChanges(INetworkService& service);
  void onBackendChanged(INetworkService& source, const NetworkState& state, NetworkChangeOrigin origin);

  std::unique_ptr<INetworkService> m_networkManager;
  std::unique_ptr<INetworkService> m_fallback;
  std::shared_ptr<INetworkService::WirelessEnabledCompletion> m_wirelessCompletion;
  ChangeCallback m_changeCallback;
  bool m_networkManagerPreferred = false;
  bool m_networkManagerAvailable = false;
};
