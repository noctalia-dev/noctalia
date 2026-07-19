#pragma once

#include "dbus/network/inetwork_service.h"
#include "shell/bar/widget.h"
#include "shell/tooltip/tooltip_content.h"

#include <string>
#include <vector>

class ExternalIpService;
class Glyph;
class Label;
class Spinner;
class SystemMonitorService;
struct wl_output;

class NetworkWidget : public Widget {
public:
  NetworkWidget(
      INetworkService* network, ExternalIpService* externalIp, SystemMonitorService* monitor, wl_output* output,
      bool showLabel, bool showVpnLabel
  );

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void syncState(Renderer& renderer);
  [[nodiscard]] std::vector<TooltipRow> buildTooltipRows() const;

  INetworkService* m_network = nullptr;
  ExternalIpService* m_externalIp = nullptr;
  SystemMonitorService* m_monitor = nullptr;
  bool m_showLabel = true;
  bool m_showVpnLabel = false;
  Glyph* m_glyph = nullptr;
  Spinner* m_spinner = nullptr;
  Label* m_label = nullptr;
  NetworkState m_lastState;
  bool m_haveLastState = false;
  bool m_isVertical = false;
  bool m_lastVertical = false;
  NetworkConnectivity m_lastRightClickTransport = NetworkConnectivity::Unknown;
};
