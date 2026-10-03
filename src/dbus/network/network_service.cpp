#include "dbus/network/network_service.h"

#include <utility>

namespace {

  void completeWireless(const std::shared_ptr<INetworkService::WirelessEnabledCompletion>& completion, bool success) {
    if (completion != nullptr) {
      if (auto callback = std::exchange(*completion, {})) {
        callback(success);
      }
    }
  }

} // namespace

NetworkService::NetworkService(
    std::unique_ptr<INetworkService> networkManager, std::unique_ptr<INetworkService> fallback
)
    : m_networkManager(std::move(networkManager)), m_fallback(std::move(fallback)) {
  m_networkManagerAvailable = m_networkManager->available();
  m_networkManagerPreferred = m_networkManagerAvailable || m_fallback == nullptr;
  watchChanges(*m_networkManager);
  if (m_fallback != nullptr) {
    watchChanges(*m_fallback);
  }
}

void NetworkService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void NetworkService::refresh() { active().refresh(); }

bool NetworkService::available() const noexcept { return active().available(); }

const NetworkState& NetworkService::state() const noexcept { return active().state(); }

bool NetworkService::hasStateSnapshot() const noexcept { return active().hasStateSnapshot(); }

const std::vector<AccessPointInfo>& NetworkService::accessPoints() const noexcept { return active().accessPoints(); }

const std::vector<VpnConnectionInfo>& NetworkService::vpnConnections() const noexcept {
  return active().vpnConnections();
}

void NetworkService::requestScan() { active().requestScan(); }

bool NetworkService::activateAccessPoint(const AccessPointInfo& ap) { return active().activateAccessPoint(ap); }

bool NetworkService::activateAccessPoint(const AccessPointInfo& ap, const std::string& psk) {
  return active().activateAccessPoint(ap, psk);
}

bool NetworkService::supportsEnterprise() const noexcept { return active().supportsEnterprise(); }

bool NetworkService::activateEnterpriseAccessPoint(
    const AccessPointInfo& ap, const network_enterprise::EnterpriseCredentials& credentials
) {
  return active().activateEnterpriseAccessPoint(ap, credentials);
}

bool NetworkService::activateVpnConnection(const VpnConnectionInfo& vpn) { return active().activateVpnConnection(vpn); }

bool NetworkService::deactivateVpnConnection(const VpnConnectionInfo& vpn) {
  return active().deactivateVpnConnection(vpn);
}

bool NetworkService::canActivateWiredConnection() const noexcept { return active().canActivateWiredConnection(); }

bool NetworkService::activateWiredConnection() { return active().activateWiredConnection(); }

bool NetworkService::canActivateCellularConnection() const noexcept { return active().canActivateCellularConnection(); }

bool NetworkService::activateCellularConnection() { return active().activateCellularConnection(); }

bool NetworkService::deactivateCellularConnection() { return active().deactivateCellularConnection(); }

void NetworkService::setWirelessEnabled(bool enabled, WirelessEnabledCompletion onComplete) {
  if (!onComplete) {
    active().setWirelessEnabled(enabled);
    return;
  }
  if (auto previous = std::exchange(m_wirelessCompletion, {})) {
    completeWireless(previous, false);
  }
  auto completion = std::make_shared<WirelessEnabledCompletion>(std::move(onComplete));
  m_wirelessCompletion = completion;
  active().setWirelessEnabled(enabled, [weakCompletion = std::weak_ptr(completion)](bool success) {
    if (auto activeCompletion = weakCompletion.lock()) {
      completeWireless(activeCompletion, success);
    }
  });
}

void NetworkService::disconnect() { active().disconnect(); }

void NetworkService::forgetSsid(const std::string& ssid) { active().forgetSsid(ssid); }

bool NetworkService::hasSavedConnection(const std::string& ssid) const { return active().hasSavedConnection(ssid); }

INetworkService& NetworkService::active() noexcept {
  return m_networkManagerPreferred ? *m_networkManager : *m_fallback;
}

const INetworkService& NetworkService::active() const noexcept {
  return m_networkManagerPreferred ? *m_networkManager : *m_fallback;
}

void NetworkService::watchChanges(INetworkService& service) {
  service.setChangeCallback([this, source = &service](const NetworkState& state, NetworkChangeOrigin origin) {
    onBackendChanged(*source, state, origin);
  });
}

void NetworkService::onBackendChanged(INetworkService& source, const NetworkState& state, NetworkChangeOrigin origin) {
  const bool networkManagerAvailable = m_networkManager->available();
  if (m_networkManagerPreferred && &source == m_networkManager.get() && !networkManagerAvailable) {
    if (auto completion = std::exchange(m_wirelessCompletion, {})) {
      completeWireless(completion, false);
    }
  }
  const bool promotion = !m_networkManagerPreferred && networkManagerAvailable && m_networkManager->hasStateSnapshot();
  const bool selectedOwnerChanged = m_networkManagerPreferred && networkManagerAvailable != m_networkManagerAvailable;
  if (promotion || selectedOwnerChanged) {
    m_networkManagerPreferred = true;
    m_networkManagerAvailable = networkManagerAvailable;
    if (auto completion = std::exchange(m_wirelessCompletion, {})) {
      completeWireless(completion, false);
    }
    if (m_changeCallback) {
      m_changeCallback(this->state(), NetworkChangeOrigin::BackendChanged);
    }
    return;
  }
  if (&source == &active() && m_changeCallback) {
    m_changeCallback(state, origin);
  }
}
