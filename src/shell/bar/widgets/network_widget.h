#pragma once

#include "dbus/network/inetwork_service.h"
#include "shell/bar/widget.h"
#include "shell/tooltip/tooltip_content.h"
#include "shell/bar/widget_custom_image.h"
#include <string>
#include <vector>

class Glyph;
class Image;
class Label;
class Spinner;
class SystemMonitorService;
struct wl_output;

class NetworkWidget : public Widget {
public:
  NetworkWidget(
      INetworkService* network, SystemMonitorService* monitor, wl_output* output, bool showLabel, bool showVpnLabel,
      bool wifiShowStrength, float labelSpacing, std::string wifiGlyphId, WidgetCustomImage wifiCustomImage,
      std::string ethernetGlyphId, WidgetCustomImage ethernetCustomImage
  );

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void syncState(Renderer& renderer);
  [[nodiscard]] std::vector<TooltipRow> buildTooltipRows() const;

  INetworkService* m_network = nullptr;
  SystemMonitorService* m_monitor = nullptr;
  bool m_showLabel = true;
  bool m_showVpnLabel = false;
  bool m_wifiShowStrength = false;
  float m_labelSpacing = 4.0f;
  std::string m_wifiGlyphId;
  WidgetCustomImage m_wifiCustomImage;
  std::string m_ethernetGlyphId;
  WidgetCustomImage m_ethernetCustomImage;
  Glyph* m_glyph = nullptr;
  Image* m_image = nullptr;
  Spinner* m_spinner = nullptr;
  Label* m_label = nullptr;
  Label* m_strengthLabel = nullptr;
  NetworkState m_lastState;
  bool m_haveLastState = false;
  bool m_isVertical = false;
  bool m_lastVertical = false;
  NetworkConnectivity m_lastRightClickTransport = NetworkConnectivity::Unknown;
};
