#include "shell/bar/widgets/network_widget.h"

#include "dbus/modem/modem_manager_service.h"
#include "dbus/network/external_ip_service.h"
#include "dbus/network/network_display.h"
#include "i18n/i18n.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "system/format_units.h"
#include "system/system_monitor_service.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace {

  constexpr auto kTooltipRefreshInterval = std::chrono::seconds(1);

  std::string labelForState(const NetworkState& s, const CellularModemInfo* modem) {
    if (s.kind == NetworkConnectivity::Wireless && s.connected && !s.ssid.empty()) {
      return s.ssid;
    }
    if (s.kind == NetworkConnectivity::Wired && s.connected) {
      return s.interfaceName.empty() ? i18n::tr("bar.widgets.network.wired") : s.interfaceName;
    }
    if (s.kind == NetworkConnectivity::Cellular && s.connected) {
      if (modem != nullptr && !modem->operatorName.empty()) {
        return modem->operatorName;
      }
      return i18n::tr("bar.widgets.network.cellular");
    }
    return {};
  }

  std::string firstActiveVpnName(const std::vector<VpnConnectionInfo>& vpns) {
    for (const auto& vpn : vpns) {
      if (vpn.active && !vpn.name.empty()) {
        return vpn.name;
      }
    }
    return {};
  }

  std::string onOffText(bool enabled) {
    return i18n::tr(enabled ? "bar.widgets.network.on" : "bar.widgets.network.off");
  }

  std::string disconnectedText(bool resolving) {
    return i18n::tr(resolving ? "bar.widgets.network.connecting" : "bar.widgets.network.not-connected");
  }

  std::string yesNoText(bool enabled) {
    return i18n::tr(enabled ? "bar.widgets.network.yes" : "bar.widgets.network.no");
  }

  std::string networkCountText(std::size_t count) {
    return i18n::trp("bar.widgets.network.networks-count", static_cast<long>(count));
  }

} // namespace

NetworkWidget::NetworkWidget(
    INetworkService* network, ExternalIpService* externalIp, SystemMonitorService* monitor, ModemManagerService* modem,
    wl_output* /*output*/, Options options
)
    : m_network(network), m_externalIp(externalIp), m_monitor(monitor), m_modem(modem), m_showLabel(options.showLabel),
      m_showVpnLabel(options.showVpnLabel), m_vpnStatusMode(options.vpnStatusMode),
      m_glyphPosition(options.glyphPosition) {
  setPeek(options.peek && options.showLabel, options.peekDelayMs);
}

void NetworkWidget::create() {
  auto area = ui::inputArea({});
  area->setTooltipProvider(
      [this] -> TooltipContent {
        std::vector<TooltipRow> rows = buildTooltipRows();
        if (rows.empty()) {
          return std::monostate{};
        }
        return TooltipContent{std::move(rows)};
      },
      kTooltipRefreshInterval
  );

  area->addChild(
      ui::glyph({
          .out = &m_vpnGlyph,
          .glyph = "shield-check",
          .glyphSize = Style::baseGlyphSize * m_contentScale,
          .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
          .visible = false,
      })
  );

  area->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = "wifi-off",
          .glyphSize = Style::baseGlyphSize * m_contentScale,
          .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
      })
  );

  // Replaces the glyph while a wired link is activating.
  area->addChild(
      ui::spinner({
          .out = &m_spinner,
          .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
          .spinnerSize = Style::baseGlyphSize * 0.8F * m_contentScale,
          .visible = false,
      })
  );

  // Always create the label node: horizontal bars honor m_showLabel, but
  // vertical bars always display a 3-char truncation under the glyph to match
  // volume/brightness.
  area->addChild(
      ui::label({
          .out = &m_label,
          .fontSize = Style::fontSizeBody * fontScale(),
          .fontWeight = labelFontWeight(),
          .fontFamily = labelFontFamily(),
      })
  );

  // VPN label for separate mode — shows VPN name next to VPN icon.
  area->addChild(
      ui::label({
          .out = &m_vpnLabel,
          .fontSize = Style::fontSizeBody * fontScale(),
          .fontWeight = labelFontWeight(),
          .fontFamily = labelFontFamily(),
          .visible = false,
      })
  );

  setRoot(std::move(area));
}

void NetworkWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  auto* rootNode = root();
  if (m_glyph == nullptr || rootNode == nullptr) {
    return;
  }
  m_isVertical = containerHeight > containerWidth;
  syncState(renderer);

  if (m_vpnGlyph != nullptr && m_vpnGlyph->visible()) {
    m_vpnGlyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
    m_vpnGlyph->measure(renderer);
  }
  m_glyph->measure(renderer);
  if (m_label != nullptr) {
    m_label->measure(renderer);
  }
  if (m_vpnLabel != nullptr && m_vpnLabel->visible()) {
    m_vpnLabel->setFontSize((m_isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * fontScale());
    m_vpnLabel->measure(renderer);
  }

  // Glyph and spinner share one slot; only one is visible.
  Node* icon =
      (m_spinner != nullptr && m_spinner->visible()) ? static_cast<Node*>(m_spinner) : static_cast<Node*>(m_glyph);

  Node* vpnIcon = m_vpnGlyph != nullptr && m_vpnGlyph->visible() ? m_vpnGlyph : nullptr;
  Node* vpnLabel = m_vpnLabel != nullptr && m_vpnLabel->visible() && m_vpnLabel->width() > 0.0F ? m_vpnLabel : nullptr;
  Node* networkLabel = m_label != nullptr && m_label->visible() && m_label->width() > 0.0F ? m_label : nullptr;
  if (vpnIcon == nullptr) {
    layoutGlyphValue({
        .glyph = icon,
        .value = networkLabel,
        .glyphPosition = m_glyphPosition,
        .vertical = m_isVertical,
        .horizontalGap = Style::spaceXs,
    });
    return;
  }
  const bool glyphAfter = m_glyphPosition == GlyphPosition::After;
  // Peek keeps both status glyphs visible and reveals their labels together.
  const std::array<Node*, 4> items = peekEnabled() ? (glyphAfter ? std::array{vpnLabel, networkLabel, vpnIcon, icon}
                                                                 : std::array{vpnIcon, icon, vpnLabel, networkLabel})
                                                   : (glyphAfter ? std::array{vpnLabel, vpnIcon, networkLabel, icon}
                                                                 : std::array{vpnIcon, vpnLabel, icon, networkLabel});
  const auto mainOf = [this](const Node* node) { return m_isVertical ? node->height() : node->width(); };
  const auto crossOf = [this](const Node* node) { return m_isVertical ? node->width() : node->height(); };
  const auto gapBetween = [&](const Node* first, const Node* second) {
    if (first == nullptr || second == nullptr) {
      return 0.0F;
    }
    if (!m_isVertical) {
      return Style::spaceXs;
    }
    if (peekEnabled()) {
      const bool firstIsIcon = first == icon || first == vpnIcon;
      const bool secondIsIcon = second == icon || second == vpnIcon;
      return firstIsIcon == secondIsIcon ? Style::spaceXs : 0.0F;
    }
    const bool firstIsVpn = first == vpnIcon || first == vpnLabel;
    const bool secondIsVpn = second == vpnIcon || second == vpnLabel;
    return firstIsVpn == secondIsVpn ? 0.0F : Style::spaceXs;
  };

  float contentMain = 0.0F;
  float cross = 0.0F;
  const Node* previous = nullptr;
  for (const Node* item : items) {
    if (item == nullptr) {
      continue;
    }
    contentMain += gapBetween(previous, item) + mainOf(item);
    cross = std::max(cross, crossOf(item));
    previous = item;
  }
  rootNode->setSize(m_isVertical ? cross : contentMain, m_isVertical ? contentMain : cross);
  const float glyphMain = mainOf(icon) + (vpnIcon != nullptr ? gapBetween(vpnIcon, icon) + mainOf(vpnIcon) : 0.0F);
  float cursor = applyPeek(contentMain, glyphMain, m_glyphPosition, m_isVertical);
  previous = nullptr;
  for (Node* item : items) {
    if (item == nullptr) {
      continue;
    }
    cursor += gapBetween(previous, item);
    float crossPos = (cross - crossOf(item)) * 0.5F;
    if (item == icon || item == vpnIcon) {
      crossPos = std::round(crossPos);
    }
    item->setPosition(m_isVertical ? crossPos : cursor, m_isVertical ? cursor : crossPos);
    cursor += mainOf(item);
    previous = item;
  }
}

void NetworkWidget::doUpdate(Renderer& renderer) { syncState(renderer); }

void NetworkWidget::syncState(Renderer& renderer) {
  if (m_glyph == nullptr || m_network == nullptr) {
    return;
  }

  const NetworkState& s = m_network->state();
  const CellularModemInfo* modem = m_modem != nullptr ? m_modem->primaryModem() : nullptr;
  const bool cellularPresent = modem != nullptr;
  const bool cellularEnabled = cellularPresent && modem->enabled();
  const std::uint8_t cellularSignal = cellularPresent ? modem->signalQuality : 0;
  const std::string cellularOperator = cellularPresent ? modem->operatorName : std::string{};
  if (m_haveLastState
      && s == m_lastState
      && m_isVertical == m_lastVertical
      && cellularPresent == m_lastCellularPresent
      && cellularEnabled == m_lastCellularEnabled
      && cellularSignal == m_lastCellularSignal
      && cellularOperator == m_lastCellularOperator) {
    return;
  }
  m_lastState = s;
  m_haveLastState = true;
  m_lastVertical = m_isVertical;
  m_lastCellularPresent = cellularPresent;
  m_lastCellularEnabled = cellularEnabled;
  m_lastCellularSignal = cellularSignal;
  m_lastCellularOperator = cellularOperator;

  const bool cellularPrimary = s.kind == NetworkConnectivity::Cellular;
  const bool showSpinner = s.kind == NetworkConnectivity::Wired && s.resolving;

  // VPN glyph (both mode): show shield icon next to network icon
  if (m_vpnGlyph != nullptr) {
    const bool showVpn = m_vpnStatusMode == VpnStatusMode::Both && s.vpnActive;
    m_vpnGlyph->setVisible(showVpn);
    if (showVpn) {
      m_vpnGlyph->setGlyph(network_display::vpnGlyph());
      m_vpnGlyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
      m_vpnGlyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
      m_vpnGlyph->measure(renderer);
    }
  }

  // Main network glyph: replace mode uses the VPN icon when active; a primary
  // cellular connection shows live ModemManager signal bars.
  m_glyph->setVisible(!showSpinner);
  if (m_vpnStatusMode == VpnStatusMode::Replace && s.vpnActive) {
    m_glyph->setGlyph(network_display::vpnGlyph());
  } else if (cellularPrimary && modem != nullptr) {
    m_glyph->setGlyph(
        modem->enabled() ? network_display::cellularGlyphForSignal(modem->signalQuality)
                         : network_display::cellularOffGlyph()
    );
  } else {
    m_glyph->setGlyph(network_display::glyphForState(s));
  }
  m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
  m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
  m_glyph->measure(renderer);

  if (m_spinner != nullptr) {
    m_spinner->setVisible(showSpinner);
    m_spinner->setSpinnerSize(Style::baseGlyphSize * 0.8F * m_contentScale);
    if (showSpinner && !m_spinner->spinning()) {
      m_spinner->start();
    } else if (!showSpinner && m_spinner->spinning()) {
      m_spinner->stop();
    }
  }

  if (m_label != nullptr) {
    const bool showLabel = m_showLabel;
    m_label->setVisible(showLabel);
    if (showLabel) {
      std::string text = labelForState(s, modem);
      // In replace mode, vpn_label overrides the network label.
      if (m_vpnStatusMode == VpnStatusMode::Replace && m_showVpnLabel && s.vpnActive) {
        if (std::string vpnName = firstActiveVpnName(m_network->vpnConnections()); !vpnName.empty()) {
          text = std::move(vpnName);
        }
      }
      if (m_isVertical && text.size() > 3) {
        text = text.substr(0, 3);
      }
      m_label->setFontSize((m_isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * fontScale());
      m_label->setText(text);
      m_label->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
      m_label->measure(renderer);
    }
  }

  // VPN label (both mode): shows VPN name next to VPN icon.
  if (m_vpnLabel != nullptr) {
    const bool showVpnLabel = m_vpnStatusMode == VpnStatusMode::Both && m_showVpnLabel && m_showLabel && s.vpnActive;
    m_vpnLabel->setVisible(showVpnLabel);
    if (showVpnLabel) {
      std::string vpnText = firstActiveVpnName(m_network->vpnConnections());
      if (m_isVertical && vpnText.size() > 3) {
        vpnText = vpnText.substr(0, 3);
      }
      m_vpnLabel->setFontSize((m_isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * fontScale());
      m_vpnLabel->setText(vpnText);
      m_vpnLabel->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
      m_vpnLabel->measure(renderer);
    }
  }

  if (auto* rootNode = root(); rootNode != nullptr) {
    rootNode->setOpacity(1.0F);
    static_cast<InputArea*>(rootNode)->requestTooltipRefresh();
  }

  requestRedraw();
}

std::vector<TooltipRow> NetworkWidget::buildTooltipRows() const {
  std::vector<TooltipRow> rows;
  if (m_network == nullptr) {
    return rows;
  }

  // Only an enabled modem earns rows on a non-cellular connection; an idle WWAN
  // card would otherwise add "Cellular: Off" to every tooltip.
  const CellularModemInfo* modem = m_modem != nullptr ? m_modem->primaryModem() : nullptr;
  const CellularModemInfo* secondaryModem = (modem != nullptr && modem->enabled()) ? modem : nullptr;

  auto appendCellularRows = [&rows](const CellularModemInfo& m) {
    rows.push_back({i18n::tr("bar.widgets.network.cellular"), cellularStateText(m.state)});
    if (!m.operatorName.empty()) {
      rows.push_back({i18n::tr("bar.widgets.network.operator"), m.operatorName});
    }
    rows.push_back({i18n::tr("bar.widgets.network.signal"), std::to_string(m.signalQuality) + "%"});
    if (const char* tech = cellularAccessTechnologyName(m.accessTechnologies); tech[0] != '\0') {
      rows.push_back({i18n::tr("bar.widgets.network.technology"), tech});
    }
  };

  const NetworkState& s = m_network->state();
  if (s.connected) {
    if (s.kind == NetworkConnectivity::Wireless && !s.ssid.empty()) {
      rows.push_back({i18n::tr("bar.widgets.network.network"), s.ssid});
      rows.push_back({i18n::tr("bar.widgets.network.signal"), std::to_string(s.signalStrength) + "%"});
      if (const char* band = network_display::wifiFrequencyBandLabel(s.frequencyMhz); band != nullptr) {
        rows.push_back({i18n::tr("bar.widgets.network.band"), band});
      }
      if (!s.interfaceName.empty()) {
        rows.push_back({i18n::tr("bar.widgets.network.interface"), s.interfaceName});
      }
    } else if (s.kind == NetworkConnectivity::Wired) {
      rows.push_back({i18n::tr("bar.widgets.network.network"), i18n::tr("bar.widgets.network.wired")});
      if (!s.interfaceName.empty()) {
        rows.push_back({i18n::tr("bar.widgets.network.interface"), s.interfaceName});
      }
    } else if (s.kind == NetworkConnectivity::Cellular) {
      rows.push_back(
          {i18n::tr("bar.widgets.network.network"),
           (modem != nullptr && !modem->operatorName.empty()) ? modem->operatorName
                                                              : i18n::tr("bar.widgets.network.cellular")}
      );
      if (modem != nullptr) {
        rows.push_back({i18n::tr("bar.widgets.network.signal"), std::to_string(modem->signalQuality) + "%"});
        if (const char* tech = cellularAccessTechnologyName(modem->accessTechnologies); tech[0] != '\0') {
          rows.push_back({i18n::tr("bar.widgets.network.technology"), tech});
        }
      }
      if (!s.interfaceName.empty()) {
        rows.push_back({i18n::tr("bar.widgets.network.interface"), s.interfaceName});
      }
    } else {
      rows.push_back({i18n::tr("bar.widgets.network.network"), i18n::tr("bar.widgets.network.connected")});
    }

    if (!s.ipv4.empty()) {
      rows.push_back({i18n::tr("bar.widgets.network.ip"), s.ipv4});
    }

    if (m_externalIp != nullptr && !m_externalIp->externalIp().empty()) {
      rows.push_back({i18n::tr("bar.widgets.network.wan-ip"), m_externalIp->externalIp()});
    }

    if (m_monitor != nullptr && m_monitor->isRunning()) {
      const SystemStats stats = m_monitor->latest();
      rows.push_back(
          {i18n::tr("bar.widgets.network.download"), FormatUnits::formatDecimalBytesPerSecond(stats.netRxBytesPerSec)}
      );
      rows.push_back(
          {i18n::tr("bar.widgets.network.upload"), FormatUnits::formatDecimalBytesPerSecond(stats.netTxBytesPerSec)}
      );
    }

    if (s.vpnActive) {
      std::string vpnLabel;
      for (const auto& vpn : m_network->vpnConnections()) {
        if (!vpn.active || vpn.name.empty()) {
          continue;
        }
        if (!vpnLabel.empty()) {
          vpnLabel += ", ";
        }
        vpnLabel += vpn.name;
      }
      rows.push_back(
          {i18n::tr("bar.widgets.network.vpn"), vpnLabel.empty() ? i18n::tr("bar.widgets.network.active") : vpnLabel}
      );
    }

    if (s.kind == NetworkConnectivity::Wireless) {
      rows.push_back({i18n::tr("bar.widgets.network.networks"), networkCountText(m_network->accessPoints().size())});
    }
    if (s.kind != NetworkConnectivity::Cellular && secondaryModem != nullptr) {
      appendCellularRows(*secondaryModem);
    }
    return rows;
  }

  rows.push_back({i18n::tr("bar.widgets.network.network"), disconnectedText(s.resolving)});
  rows.push_back({i18n::tr("bar.widgets.network.wifi"), onOffText(s.wirelessEnabled)});
  if (s.scanning) {
    rows.push_back({i18n::tr("bar.widgets.network.scanning"), yesNoText(s.scanning)});
  }
  if (s.wirelessEnabled) {
    rows.push_back({i18n::tr("bar.widgets.network.networks"), networkCountText(m_network->accessPoints().size())});
  }
  if (secondaryModem != nullptr) {
    appendCellularRows(*secondaryModem);
  }
  if (s.vpnActive) {
    rows.push_back({i18n::tr("bar.widgets.network.vpn"), i18n::tr("bar.widgets.network.active")});
  }
  return rows;
}
