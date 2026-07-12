#pragma once

#include "dbus/bluetooth/bluetooth_agent.h"
#include "dbus/bluetooth/bluetooth_service.h"
#include "shell/control_center/tab.h"

#include <string>
#include <unordered_map>
#include <vector>

class BluetoothDeviceRow;
class Button;
class Flex;
class Input;
class Label;
class ScrollView;
class Spinner;
class Toggle;

class BluetoothTab : public Tab {
public:
  BluetoothTab(BluetoothService* service, BluetoothAgent* agent);
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
  // Pushes live metric values into the existing rows. Returns true if any changed.
  bool syncDeviceRows();
  [[nodiscard]] std::string structureKey(const std::vector<BluetoothDeviceInfo>& devices) const;

  BluetoothService* m_service = nullptr;
  BluetoothAgent* m_agent = nullptr;

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

  std::unordered_map<std::string, BluetoothDeviceRow*> m_deviceRows;

  std::string m_lastStructureKey;
  float m_lastListWidth = -1.0f;
};
