#include "shell/bar/widgets/network_widget.h"

#include "dbus/network/network_glyphs.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <memory>

namespace {

  std::string labelForState(const NetworkState& s) {
    if (s.kind == NetworkConnectivity::Wireless && s.connected && !s.ssid.empty()) {
      return s.ssid;
    }
    if (s.kind == NetworkConnectivity::Wired && s.connected) {
      return s.interfaceName.empty() ? i18n::tr("bar.widgets.network.wired") : s.interfaceName;
    }
    return {};
  }

} // namespace

NetworkWidget::NetworkWidget(INetworkService* network, wl_output* /*output*/, bool showLabel)
    : m_network(network), m_showLabel(showLabel) {}

void NetworkWidget::create() {
  auto area = std::make_unique<InputArea>();
  area->setOnClick([this](const InputArea::PointerData& /*data*/) { requestPanelToggle("control-center", "network"); });

  area->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = "wifi-off",
          .glyphSize = Style::barGlyphSize * m_contentScale,
          .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
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

  const bool labelVisible = m_label != nullptr && m_label->width() > 0.0f && m_label->visible();
  if (m_isVertical && labelVisible) {
    const float w = std::max(m_glyph->width(), m_label->width());
    m_glyph->setPosition(std::round((w - m_glyph->width()) * 0.5f), 0.0f);
    m_label->setPosition(std::round((w - m_label->width()) * 0.5f), m_glyph->height());
    rootNode->setSize(w, m_glyph->height() + m_label->height());
  } else {
    const float h = labelVisible ? std::max(m_glyph->height(), m_label->height()) : m_glyph->height();
    m_glyph->setPosition(0.0f, std::round((h - m_glyph->height()) * 0.5f));
    float totalWidth = m_glyph->width();
    if (labelVisible) {
      m_label->setPosition(m_glyph->width() + Style::spaceXs, std::round((h - m_label->height()) * 0.5f));
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

  m_glyph->setGlyph(network_glyphs::glyphForState(s));
  m_glyph->setGlyphSize(Style::barGlyphSize * m_contentScale);
  m_glyph->setColor(
      s.connected ? widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface))
                  : colorSpecFromRole(ColorRole::OnSurfaceVariant)
  );
  m_glyph->measure(renderer);

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

    auto* area = static_cast<InputArea*>(rootNode);
    if (s.connected) {
      std::vector<TooltipRow> rows;
      if (s.kind == NetworkConnectivity::Wireless && !s.ssid.empty()) {
        rows.push_back({"Network", s.ssid});
        rows.push_back({"Signal", std::to_string(s.signalStrength) + "%"});
      } else if (s.kind == NetworkConnectivity::Wired) {
        rows.push_back({"Network", s.interfaceName.empty() ? "Wired" : s.interfaceName});
      }
      if (!s.ipv4.empty()) {
        rows.push_back({"IP", s.ipv4});
      }
      if (s.vpnActive) {
        rows.push_back({"VPN", "Active"});
      }
      area->setTooltip(std::move(rows));
    } else {
      area->clearTooltip();
    }
  }

  requestRedraw();
}
