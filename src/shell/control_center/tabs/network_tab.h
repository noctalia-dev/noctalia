#pragma once

#include "core/timer_manager.h"
#include "dbus/network/network_secret_agent.h"
#include "dbus/network/network_types.h"
#include "security/secure_buffer.h"
#include "shell/control_center/tab.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class AccessPointRow;
class Button;
class ExternalIpService;
class Flex;
class Glyph;
class Input;
class Label;
class ScrollView;
class Spinner;
class Toggle;
class INetworkService;

class NetworkTab : public Tab {
public:
  NetworkTab(INetworkService* network, NetworkSecretAgent* secrets, ExternalIpService* externalIp);
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
  void requestWirelessEnabled(bool enabled);
  void handleWirelessEnabledCompletion(std::uint64_t generation, bool success);
  void requestCellularEnabled(bool enabled);
  void handleCellularEnabledCompletion(std::uint64_t generation, bool success);
  void submitCellularSetup();
  void closeCellularSetup();
  void rebuildApList(Renderer& renderer);
  // Pushes live signal values into the existing rows. Returns true if any changed.
  bool syncApRows();
  bool syncCellularRows();
  void syncPasswordCard();
  void showPasswordPrompt(const NetworkSecretAgent::SecretRequest& request);
  void showPasswordPrompt(const AccessPointInfo& ap);
  void submitPasswordPrompt(const std::string& value);
  void cancelPasswordPrompt();
  void clearPasswordPrompt();
  [[nodiscard]] std::string structureKey(
      const std::vector<AccessPointInfo>& aps, const std::vector<VpnConnectionInfo>& vpns,
      const std::vector<CellularConnectionInfo>& cellular
  ) const;

  INetworkService* m_network = nullptr;
  NetworkSecretAgent* m_secrets = nullptr;
  ExternalIpService* m_externalIpService = nullptr;

  Flex* m_rootLayout = nullptr;
  Flex* m_currentCard = nullptr;
  Label* m_currentTitle = nullptr;
  Label* m_currentDetail = nullptr;
  Flex* m_passwordCard = nullptr;
  Label* m_passwordTitle = nullptr;
  Input* m_passwordInput = nullptr;
  Button* m_passwordRevealButton = nullptr;
  Button* m_passwordSubmitButton = nullptr;
  Spinner* m_passwordSubmitSpinner = nullptr;
  bool m_passwordRevealed = false;
  ScrollView* m_listScroll = nullptr;
  Flex* m_list = nullptr;

  Button* m_rescanButton = nullptr;
  Toggle* m_wifiToggle = nullptr;
  Toggle* m_cellularToggle = nullptr;
  Input* m_cellularNameInput = nullptr;
  Input* m_cellularApnInput = nullptr;
  Flex* m_currentRow = nullptr;
  Button* m_disconnectButton = nullptr;
  Spinner* m_scanSpinner = nullptr;
  bool m_vpnVisible = true;
  bool m_cellularSetupVisible = false;
  std::string m_cellularSetupName;
  std::string m_cellularSetupApn;

  std::unordered_map<std::string, AccessPointRow*> m_apRows;
  struct CellularRowMetrics {
    Glyph* glyph = nullptr;
    Label* value = nullptr;
  };
  std::vector<CellularRowMetrics> m_cellularRows;

  std::string m_lastStructureKey;
  float m_lastListWidth = -1.0F;

  bool m_hasPendingSecret = false;
  bool m_secretSubmitting = false;
  std::chrono::steady_clock::time_point m_secretSubmittingSince;
  NetworkSecretAgent::SecretKind m_pendingSecretKind = NetworkSecretAgent::SecretKind::WifiPsk;
  std::string m_pendingSecretName;
  std::string m_pendingSecretConnectionPath;
  security::SecureBuffer m_pendingSimPin;
  std::optional<AccessPointInfo> m_pendingAccessPoint;
  bool m_active = false;

  // Connect/disconnect stays disabled from click until the state flips (or a
  // timeout), so a click on stale state cannot fire the inverse action.
  bool m_actionPending = false;
  bool m_actionPendingConnected = false;
  std::chrono::steady_clock::time_point m_actionPendingSince;

  bool m_wifiTogglePending = false;
  bool m_wifiToggleTarget = false;
  bool m_wifiToggleWriteComplete = false;
  bool m_wifiToggleTargetObserved = false;
  std::uint64_t m_wifiToggleRequestGeneration = 0;
  bool m_cellularTogglePending = false;
  bool m_cellularToggleTarget = false;
  bool m_cellularToggleWriteComplete = false;
  bool m_cellularToggleTargetObserved = false;
  std::uint64_t m_cellularToggleRequestGeneration = 0;

  Timer m_actionPendingTimer;

  static constexpr std::chrono::seconds kActionPendingTimeout = std::chrono::seconds(6);
  static constexpr std::chrono::seconds kSecretSubmittingTimeout = std::chrono::seconds(45);
};
