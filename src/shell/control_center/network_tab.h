#pragma once

#include "dbus/network/inetwork_service.h"
#include "dbus/network/network_secret_agent.h"
#include "shell/control_center/tab.h"

#include <optional>
#include <string>
#include <vector>

class Button;
class Flex;
class Input;
class Label;
class ScrollView;
class Spinner;
class Toggle;

class NetworkTab : public Tab {
public:
  NetworkTab(INetworkService* network, NetworkSecretAgent* secrets);
  ~NetworkTab() override;

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void setActive(bool active) override;
  void onClose() override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;

  void syncCurrentCard();
  void rebuildApList(Renderer& renderer);
  void syncPasswordCard();
  void showPasswordPrompt(const NetworkSecretAgent::SecretRequest& request);
  void showPasswordPrompt(const AccessPointInfo& ap);
  void submitPasswordPrompt(const std::string& value);
  void cancelPasswordPrompt();
  void clearPasswordPrompt();
  [[nodiscard]] std::string
  structureKey(const std::vector<AccessPointInfo>& aps, const std::vector<VpnConnectionInfo>& vpns) const;
  [[nodiscard]] std::string apRowsKey(const std::vector<AccessPointInfo>& aps) const;

  INetworkService* m_network = nullptr;
  NetworkSecretAgent* m_secrets = nullptr;

  Flex* m_rootLayout = nullptr;
  Flex* m_currentCard = nullptr;
  Label* m_currentTitle = nullptr;
  Label* m_currentDetail = nullptr;
  Flex* m_passwordCard = nullptr;
  Label* m_passwordTitle = nullptr;
  Input* m_passwordInput = nullptr;
  Button* m_passwordRevealButton = nullptr;
  bool m_passwordRevealed = false;
  Flex* m_listCard = nullptr;
  ScrollView* m_listScroll = nullptr;
  Flex* m_list = nullptr;

  Button* m_rescanButton = nullptr;
  Toggle* m_wifiToggle = nullptr;
  Flex* m_currentRow = nullptr;
  Button* m_disconnectButton = nullptr;
  Spinner* m_scanSpinner = nullptr;
  bool m_vpnVisible = true;

  Flex* m_vpnSection = nullptr;
  Flex* m_apRows = nullptr;

  std::string m_lastStructureKey;
  std::string m_lastApRowsKey;
  float m_lastListWidth = -1.0f;

  bool m_hasPendingSecret = false;
  std::string m_pendingSsid;
  std::optional<AccessPointInfo> m_pendingAccessPoint;
  bool m_active = false;
};
