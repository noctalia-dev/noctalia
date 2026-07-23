#include "shell/bar/widgets/network_widget.h"

#include "dbus/network/external_ip_service.h"
#include "dbus/network/network_glyphs.h"
#include "i18n/i18n.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "system/format_units.h"
#include "system/system_monitor_service.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <chrono>
#include <linux/input-event-codes.h>
#include <memory>
#include <string>
#include <vector>

namespace {

  constexpr auto kTooltipRefreshInterval = std::chrono::seconds(1);

  std::string labelForState(const NetworkState& s) {
    if (s.kind == NetworkConnectivity::Wireless && s.connected && !s.ssid.empty()) {
      return s.ssid;
    }
    if (s.kind == NetworkConnectivity::Wired && s.connected) {
      return s.interfaceName.empty() ? i18n::tr("bar.widgets.network.wired") : s.interfaceName;
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
    INetworkService* network, ExternalIpService* externalIp, SystemMonitorService* monitor, wl_output* /*output*/,
    bool showLabel, bool showVpnLabel, std::string vpnStatusMode
)
    : m_network(network), m_externalIp(externalIp), m_monitor(monitor), m_showLabel(showLabel),
      m_showVpnLabel(showVpnLabel) {
  if (vpnStatusMode == "both") {
    m_vpnStatusMode = VpnStatusMode::Both;
  } else if (vpnStatusMode == "hidden") {
    m_vpnStatusMode = VpnStatusMode::Hidden;
  } else {
    m_vpnStatusMode = VpnStatusMode::Replace;
  }
}

void NetworkWidget::create() {
  auto area = std::make_unique<InputArea>();
  area->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}));
  area->setOnClick([this](const InputArea::PointerData& data) {
    if (data.button == BTN_RIGHT) {
      if (m_network == nullptr) {
        return;
      }
      const NetworkState& s = m_network->state();
      if (s.kind == NetworkConnectivity::Wireless && (s.connected || s.resolving)) {
        m_lastRightClickTransport = NetworkConnectivity::Wireless;
        m_network->setWirelessEnabled(false);
      } else if (s.kind == NetworkConnectivity::Wired && (s.connected || s.resolving)) {
        m_lastRightClickTransport = NetworkConnectivity::Wired;
        m_network->disconnect();
      } else if (m_lastRightClickTransport == NetworkConnectivity::Wireless) {
        m_lastRightClickTransport = NetworkConnectivity::Unknown;
        m_network->setWirelessEnabled(true);
      } else if (m_lastRightClickTransport == NetworkConnectivity::Wired) {
        m_lastRightClickTransport = NetworkConnectivity::Unknown;
        m_network->activateWiredConnection();
      } else if (!s.wirelessEnabled) {
        m_network->setWirelessEnabled(true);
      } else if (m_network->canActivateWiredConnection()) {
        m_network->activateWiredConnection();
      }
      return;
    }
    if (data.button == BTN_LEFT) {
      requestPanelToggle("control-center", "network");
    }
  });
  area->setTooltipProvider(
      [this]() -> TooltipContent {
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
          .spinnerSize = Style::baseGlyphSize * 0.8f * m_contentScale,
          .visible = false,
      })
  );

  // Always create the label node: horizontal bars honor m_showLabel, but
  // vertical bars always display a 3-char truncation under the glyph to match
  // volume/brightness.
  area->addChild(
      ui::label({
          .out = &m_label,
          .fontSize = Style::fontSizeBody * m_contentScale,
          .fontWeight = labelFontWeight(),
          .fontFamily = labelFontFamily(),
      })
  );

  // VPN label for separate mode — shows VPN name next to VPN icon.
  area->addChild(
      ui::label({
          .out = &m_vpnLabel,
          .fontSize = Style::fontSizeBody * m_contentScale,
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
    m_vpnLabel->setFontSize((m_isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * m_contentScale);
    m_vpnLabel->measure(renderer);
  }

  // Glyph and spinner share one slot; only one is visible.
  Node* icon =
      (m_spinner != nullptr && m_spinner->visible()) ? static_cast<Node*>(m_spinner) : static_cast<Node*>(m_glyph);

  const bool vpnVisible = m_vpnGlyph != nullptr && m_vpnGlyph->visible();
  const bool vpnLabelVisible = m_vpnLabel != nullptr && m_vpnLabel->visible();
  const bool networkLabelVisible = m_label != nullptr && m_label->width() > 0.0f && m_label->visible();

  if (m_isVertical) {
    // Vertical: stack everything centered, top to bottom
    // VPN glyph → VPN label → network glyph → network label
    const float w = [&]() {
      float maxW = icon->width();
      if (vpnVisible) {
        maxW = std::max(maxW, m_vpnGlyph->width());
      }
      if (vpnLabelVisible) {
        maxW = std::max(maxW, m_vpnLabel->width());
      }
      if (networkLabelVisible) {
        maxW = std::max(maxW, m_label->width());
      }
      return maxW;
    }();

    float y = 0.0f;
    if (vpnVisible) {
      m_vpnGlyph->setPosition(std::round((w - m_vpnGlyph->width()) * 0.5f), y);
      y += m_vpnGlyph->height();
    }
    if (vpnLabelVisible) {
      m_vpnLabel->setPosition(std::round((w - m_vpnLabel->width()) * 0.5f), y);
      y += m_vpnLabel->height();
    }
    if (vpnVisible) {
      y += Style::spaceXs;
    }
    icon->setPosition(std::round((w - icon->width()) * 0.5f), y);
    y += icon->height();
    if (networkLabelVisible) {
      m_label->setPosition(std::round((w - m_label->width()) * 0.5f), y);
      y += m_label->height();
    }
    rootNode->setSize(w, y);
  } else {
    // Horizontal: vpnGlyph + vpnLabel | space | networkGlyph + networkLabel
    const float vpnGroupWidth =
        vpnVisible ? m_vpnGlyph->width() + (vpnLabelVisible ? Style::spaceXs + m_vpnLabel->width() : 0.0f) : 0.0f;
    const float networkGroupWidth = icon->width() + (networkLabelVisible ? Style::spaceXs + m_label->width() : 0.0f);
    const float gap = vpnVisible ? Style::spaceXs : 0.0f;
    const float totalWidth = vpnGroupWidth + gap + networkGroupWidth;
    const float h = [&]() {
      float maxH = icon->height();
      if (vpnVisible) {
        maxH = std::max(maxH, m_vpnGlyph->height());
      }
      if (vpnLabelVisible) {
        maxH = std::max(maxH, m_vpnLabel->height());
      }
      if (networkLabelVisible) {
        maxH = std::max(maxH, m_label->height());
      }
      return maxH;
    }();

    float x = 0.0f;
    if (vpnVisible) {
      m_vpnGlyph->setPosition(x, std::round((h - m_vpnGlyph->height()) * 0.5f));
      x += m_vpnGlyph->width();
      if (vpnLabelVisible) {
        m_vpnLabel->setPosition(x + Style::spaceXs, std::round((h - m_vpnLabel->height()) * 0.5f));
        x += Style::spaceXs + m_vpnLabel->width();
      }
    }
    x += gap;
    icon->setPosition(x, std::round((h - icon->height()) * 0.5f));
    x += icon->width();
    if (networkLabelVisible) {
      m_label->setPosition(x + Style::spaceXs, std::round((h - m_label->height()) * 0.5f));
    }
    rootNode->setSize(totalWidth, h);
  }
}

void NetworkWidget::doUpdate(Renderer& renderer) { syncState(renderer); }

void NetworkWidget::syncState(Renderer& renderer) {
  if (m_glyph == nullptr || m_network == nullptr) {
    return;
  }

  const NetworkState& s = m_network->state();
  if (m_haveLastState && s == m_lastState && m_isVertical == m_lastVertical) {
    return;
  }
  m_lastState = s;
  m_haveLastState = true;
  m_lastVertical = m_isVertical;

  const bool showSpinner = s.kind == NetworkConnectivity::Wired && s.resolving;

  // VPN glyph (both mode): show shield icon next to network icon
  if (m_vpnGlyph != nullptr) {
    const bool showVpn = m_vpnStatusMode == VpnStatusMode::Both && s.vpnActive;
    m_vpnGlyph->setVisible(showVpn);
    if (showVpn) {
      m_vpnGlyph->setGlyph(network_glyphs::vpnGlyph());
      m_vpnGlyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
      m_vpnGlyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
      m_vpnGlyph->measure(renderer);
    }
  }

  // Main network glyph: replace mode uses the VPN icon when active.
  m_glyph->setVisible(!showSpinner);
  if (m_vpnStatusMode == VpnStatusMode::Replace && s.vpnActive) {
    m_glyph->setGlyph(network_glyphs::vpnGlyph());
  } else {
    m_glyph->setGlyph(network_glyphs::glyphForState(s));
  }
  m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
  m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
  m_glyph->measure(renderer);

  if (m_spinner != nullptr) {
    m_spinner->setVisible(showSpinner);
    m_spinner->setSpinnerSize(Style::baseGlyphSize * 0.8f * m_contentScale);
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
      std::string text = labelForState(s);
      // In replace mode, vpn_label overrides the network label.
      if (m_vpnStatusMode == VpnStatusMode::Replace && m_showVpnLabel && s.vpnActive) {
        if (std::string vpnName = firstActiveVpnName(m_network->vpnConnections()); !vpnName.empty()) {
          text = std::move(vpnName);
        }
      }
      if (m_isVertical && text.size() > 3) {
        text = text.substr(0, 3);
      }
      m_label->setFontSize((m_isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * m_contentScale);
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
      m_vpnLabel->setFontSize((m_isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * m_contentScale);
      m_vpnLabel->setText(vpnText);
      m_vpnLabel->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
      m_vpnLabel->measure(renderer);
    }
  }

  if (auto* rootNode = root(); rootNode != nullptr) {
    rootNode->setOpacity(1.0f);
    static_cast<InputArea*>(rootNode)->requestTooltipRefresh();
  }

  requestRedraw();
}

std::vector<TooltipRow> NetworkWidget::buildTooltipRows() const {
  std::vector<TooltipRow> rows;
  if (m_network == nullptr) {
    return rows;
  }

  const NetworkState& s = m_network->state();
  if (s.connected) {
    if (s.kind == NetworkConnectivity::Wireless && !s.ssid.empty()) {
      rows.push_back({i18n::tr("bar.widgets.network.network"), s.ssid});
      rows.push_back({i18n::tr("bar.widgets.network.signal"), std::to_string(s.signalStrength) + "%"});
      if (!s.interfaceName.empty()) {
        rows.push_back({i18n::tr("bar.widgets.network.interface"), s.interfaceName});
      }
    } else if (s.kind == NetworkConnectivity::Wired) {
      rows.push_back({i18n::tr("bar.widgets.network.network"), i18n::tr("bar.widgets.network.wired")});
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
  if (s.vpnActive) {
    rows.push_back({i18n::tr("bar.widgets.network.vpn"), i18n::tr("bar.widgets.network.active")});
  }
  return rows;
}
