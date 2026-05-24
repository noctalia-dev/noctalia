#pragma once

#include "dbus/bluetooth/bluetooth_service.h"
#include "shell/bar/widget.h"

#include <string>

class Glyph;
class Label;
struct wl_output;

class BluetoothWidget : public Widget {
public:
  BluetoothWidget(
      BluetoothService* bluetooth, wl_output* output, bool showLabel, bool hideWhenNoConnectedDevice = false
  );

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void syncState(Renderer& renderer);
  void syncWidgetVisibility(bool showWidget);

  BluetoothService* m_bluetooth = nullptr;
  bool m_showLabel = false;
  bool m_hideWhenNoConnectedDevice = false;
  Glyph* m_glyph = nullptr;
  Label* m_label = nullptr;
  BluetoothState m_lastState;
  bool m_haveLastState = false;
  int m_lastConnectedCount = -1;
  std::string m_lastConnectedAlias;
};
