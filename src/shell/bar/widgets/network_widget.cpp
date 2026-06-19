#include "shell/bar/widgets/network_widget.h"

#include "dbus/network/network_glyphs.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "system/format_units.h"
#include "system/system_monitor_service.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <chrono>
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

  std::string onOffText(bool enabled) { return enabled ? "On" : "Off"; }

  std::string disconnectedText(bool resolving) { return resolving ? "Connecting" : "Not connected"; }

  std::string yesNoText(bool enabled) { return enabled ? "Yes" : "No"; }

  std::string networkCountText(std::size_t count) {
    return std::to_string(count) + (count == 1 ? " network" : " networks");
  }

} // namespace

NetworkWidget::NetworkWidget(
    INetworkService* network, SystemMonitorService* monitor, wl_output* /*output*/, bool showLabel
)
    : m_network(network), m_monitor(monitor), m_showLabel(showLabel) {}

void NetworkWidget::create() {
  auto area = std::make_unique<InputArea>();
  area->setOnClick([this](const InputArea::PointerData& /*data*/) { requestPanelToggle("control-center", "network"); });
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
          .fontFamily = labelFontFamily(),
          .fontWeight = labelFontWeight(),
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

  m_glyph->measure(renderer);
  if (m_label != nullptr) {
    m_label->measure(renderer);
  }

  // Glyph and spinner share one slot; only one is visible.
  Node* icon =
      (m_spinner != nullptr && m_spinner->visible()) ? static_cast<Node*>(m_spinner) : static_cast<Node*>(m_glyph);

  const bool labelVisible = m_label != nullptr && m_label->width() > 0.0f && m_label->visible();
  if (m_isVertical && labelVisible) {
    const float w = std::max(icon->width(), m_label->width());
    icon->setPosition(std::round((w - icon->width()) * 0.5f), 0.0f);
    m_label->setPosition(std::round((w - m_label->width()) * 0.5f), icon->height());
    rootNode->setSize(w, icon->height() + m_label->height());
  } else {
    const float h = labelVisible ? std::max(icon->height(), m_label->height()) : icon->height();
    icon->setPosition(0.0f, std::round((h - icon->height()) * 0.5f));
    float totalWidth = icon->width();
    if (labelVisible) {
      m_label->setPosition(icon->width() + Style::spaceXs, std::round((h - m_label->height()) * 0.5f));
      totalWidth = m_label->x() + m_label->width();
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

  m_glyph->setVisible(!showSpinner);
  m_glyph->setGlyph(network_glyphs::glyphForState(s));
  m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
  m_glyph->setColor(
      s.connected ? widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface))
                  : colorSpecFromRole(ColorRole::OnSurfaceVariant)
  );
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
      if (m_isVertical && text.size() > 3) {
        text = text.substr(0, 3);
      }
      m_label->setFontSize((m_isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * m_contentScale);
      m_label->setText(text);
      m_label->setColor(
          s.connected ? widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface))
                      : colorSpecFromRole(ColorRole::OnSurfaceVariant)
      );
      m_label->measure(renderer);
    }
  }

  if (auto* rootNode = root(); rootNode != nullptr) {
    rootNode->setOpacity(s.connected ? 1.0f : 0.55f);
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
      rows.push_back({"Network", s.ssid});
      rows.push_back({"Signal", std::to_string(s.signalStrength) + "%"});
      if (!s.interfaceName.empty()) {
        rows.push_back({"Interface", s.interfaceName});
      }
    } else if (s.kind == NetworkConnectivity::Wired) {
      rows.push_back({"Network", "Wired"});
      if (!s.interfaceName.empty()) {
        rows.push_back({"Interface", s.interfaceName});
      }
    } else {
      rows.push_back({"Network", "Connected"});
    }

    if (!s.ipv4.empty()) {
      rows.push_back({"IP", s.ipv4});
    }

    if (m_monitor != nullptr && m_monitor->isRunning()) {
      const SystemStats stats = m_monitor->latest();
      rows.push_back({"Download", FormatUnits::formatDecimalBytesPerSecond(stats.netRxBytesPerSec)});
      rows.push_back({"Upload", FormatUnits::formatDecimalBytesPerSecond(stats.netTxBytesPerSec)});
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
      rows.push_back({"VPN", vpnLabel.empty() ? "Active" : vpnLabel});
    }

    if (s.kind == NetworkConnectivity::Wireless) {
      rows.push_back({"Networks", networkCountText(m_network->accessPoints().size())});
    }
    return rows;
  }

  rows.push_back({"Network", disconnectedText(s.resolving)});
  rows.push_back({"Wi-Fi", onOffText(s.wirelessEnabled)});
  if (s.scanning) {
    rows.push_back({"Scanning", yesNoText(s.scanning)});
  }
  if (s.wirelessEnabled) {
    rows.push_back({"Networks", networkCountText(m_network->accessPoints().size())});
  }
  if (s.vpnActive) {
    rows.push_back({"VPN", "Active"});
  }
  return rows;
}
