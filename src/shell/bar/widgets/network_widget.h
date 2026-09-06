#pragma once

#include "dbus/network/inetwork_service.h"
#include "shell/bar/widget.h"
#include "shell/tooltip/tooltip_content.h"

#include <string>
#include <vector>

class ExternalIpService;
class Glyph;
class Label;
class ModemManagerService;
class Spinner;
class SystemMonitorService;
struct wl_output;

enum class VpnStatusMode : std::uint8_t {
  Both,    // Show VPN icon next to network icon
  Replace, // Replace network icon with VPN icon when active
  Hidden,  // Don't show VPN icon
};

class NetworkWidget : public Widget {
public:
  struct Options {
    VpnStatusMode vpnStatusMode = VpnStatusMode::Replace;
    bool showLabel = true;
    bool showVpnLabel = false;
  };

  NetworkWidget(
      INetworkService* network, ExternalIpService* externalIp, SystemMonitorService* monitor,
      ModemManagerService* modem, wl_output* output, Options options
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
  ModemManagerService* m_modem = nullptr;
  bool m_showLabel = true;
  bool m_showVpnLabel = false;
  VpnStatusMode m_vpnStatusMode = VpnStatusMode::Replace;
  Glyph* m_glyph = nullptr;
  Glyph* m_vpnGlyph = nullptr;
  Spinner* m_spinner = nullptr;
  Label* m_label = nullptr;
  Label* m_vpnLabel = nullptr;
  NetworkState m_lastState;
  bool m_haveLastState = false;
  bool m_lastCellularPresent = false;
  bool m_lastCellularEnabled = false;
  std::uint8_t m_lastCellularSignal = 0;
  std::string m_lastCellularOperator;
  bool m_isVertical = false;
  bool m_lastVertical = false;
};
