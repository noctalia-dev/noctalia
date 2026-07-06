#pragma once

#include "core/timer_manager.h"
#include "dbus/network/network_secret_agent.h"
#include "dbus/network/network_types.h"
#include "shell/control_center/tab.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Button;
class ConfigService;
class Flex;
class HttpClient;
class Input;
class Label;
class ScrollView;
class Spinner;
class Toggle;
class INetworkService;

class NetworkTab : public Tab {
public:
  NetworkTab(INetworkService* network, NetworkSecretAgent* secrets, HttpClient* http, ConfigService* config);
  ~NetworkTab() override;

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void setActive(bool active) override;
  void onClose() override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;

  void syncCurrentCard();
  void beginPendingAction(bool wasConnected);
  [[nodiscard]] bool externalIpEnabled() const;
  void maybeScheduleExternalIpProbe();
  void probeExternalIpNow();
  void rebuildApList(Renderer& renderer);
  void syncPasswordCard();
  void showPasswordPrompt(const NetworkSecretAgent::SecretRequest& request);
  void showPasswordPrompt(const AccessPointInfo& ap);
  void submitPasswordPrompt(const std::string& value);
  void cancelPasswordPrompt();
  void clearPasswordPrompt();
  [[nodiscard]] std::string
  structureKey(const std::vector<AccessPointInfo>& aps, const std::vector<VpnConnectionInfo>& vpns) const;

  INetworkService* m_network = nullptr;
  NetworkSecretAgent* m_secrets = nullptr;
  HttpClient* m_http = nullptr;
  ConfigService* m_config = nullptr;

  Flex* m_rootLayout = nullptr;
  Flex* m_currentCard = nullptr;
  Label* m_currentTitle = nullptr;
  Label* m_currentDetail = nullptr;
  Flex* m_passwordCard = nullptr;
  Label* m_passwordTitle = nullptr;
  Input* m_passwordInput = nullptr;
  Button* m_passwordRevealButton = nullptr;
  bool m_passwordRevealed = false;
  ScrollView* m_listScroll = nullptr;
  Flex* m_list = nullptr;

  Button* m_rescanButton = nullptr;
  Toggle* m_wifiToggle = nullptr;
  Flex* m_currentRow = nullptr;
  Button* m_disconnectButton = nullptr;
  Spinner* m_scanSpinner = nullptr;
  bool m_vpnVisible = true;

  std::string m_lastStructureKey;
  float m_lastListWidth = -1.0f;

  bool m_hasPendingSecret = false;
  std::string m_pendingSsid;
  std::optional<AccessPointInfo> m_pendingAccessPoint;
  bool m_active = false;

  // Connect/disconnect stays disabled from click until the state flips (or a
  // timeout), so a click on stale state cannot fire the inverse action.
  bool m_actionPending = false;
  bool m_actionPendingConnected = false;
  std::chrono::steady_clock::time_point m_actionPendingSince;

  // External IP, probed while the tab is active and connected. Keyed on the
  // local IPv4 + VPN tunnel state; a key change clears the shown value and
  // re-probes after a settle delay, then confirms once (the first probe after
  // a transition can race the route change and report the old address).
  std::string m_externalIp;
  std::string m_externalIpKey;          // key of the last completed probe
  std::string m_externalIpConfirmedKey; // key that already got its confirmation probe
  std::chrono::steady_clock::time_point m_externalIpFetchedAt;
  bool m_externalIpFetchInFlight = false;
  Timer m_externalIpTimer;
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
};
