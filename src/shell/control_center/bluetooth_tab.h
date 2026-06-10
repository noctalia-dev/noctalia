#pragma once

#include "dbus/bluetooth/bluetooth_agent.h"
#include "dbus/bluetooth/bluetooth_service.h"
#include "dbus/upower/upower_service.h"
#include "shell/control_center/tab.h"

#include <string>
#include <vector>

class Button;
class Flex;
class Input;
class Label;
class ScrollView;
class Spinner;
class Toggle;

class BluetoothTab : public Tab {
public:
  BluetoothTab(BluetoothService* service, BluetoothAgent* agent, UPowerService* upower);
  ~BluetoothTab() override;

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;
  void setActive(bool active) override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;

  void syncHeader();
  void syncPairingCard();
  void rebuildDeviceList(Renderer& renderer);
  [[nodiscard]] std::string listKey() const;

  BluetoothService* m_service = nullptr;
  BluetoothAgent* m_agent = nullptr;
  UPowerService* m_upower = nullptr;

  Flex* m_rootLayout = nullptr;

  Flex* m_pairingCard = nullptr;
  Label* m_pairingTitle = nullptr;
  Label* m_pairingDetail = nullptr;
  Label* m_pairingCode = nullptr;
  Flex* m_pairingInputRow = nullptr;
  Input* m_pairingInput = nullptr;
  Flex* m_pairingButtonRow = nullptr;
  Button* m_pairingAccept = nullptr;
  Button* m_pairingReject = nullptr;

  ScrollView* m_listScroll = nullptr;
  Flex* m_list = nullptr;

  Toggle* m_powerToggle = nullptr;
  Toggle* m_discoverableToggle = nullptr;
  Button* m_rescanButton = nullptr;
  Spinner* m_scanSpinner = nullptr;

  std::string m_lastListKey;
  float m_lastListWidth = -1.0f;
};
