#include "shell/bar/widgets/bluetooth_widget.h"

#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

  const char* glyphForState(const BluetoothState& s, int connectedCount) {
    if (!s.adapterPresent || !s.powered) {
      return "bluetooth-off";
    }
    if (connectedCount > 0) {
      return "bluetooth-connected";
    }
    return "bluetooth";
  }

  std::string firstConnectedAlias(const std::vector<BluetoothDeviceInfo>& devices) {
    for (const auto& d : devices) {
      if (d.connected) {
        return d.alias;
      }
    }
    return {};
  }

  int connectedCount(const std::vector<BluetoothDeviceInfo>& devices) {
    int count = 0;
    for (const auto& d : devices) {
      if (d.connected) {
        ++count;
      }
    }
    return count;
  }

} // namespace

BluetoothWidget::BluetoothWidget(
    BluetoothService* bluetooth, wl_output* /*output*/, bool showLabel, bool hideWhenNoConnectedDevice
)
    : m_bluetooth(bluetooth), m_showLabel(showLabel), m_hideWhenNoConnectedDevice(hideWhenNoConnectedDevice) {}

void BluetoothWidget::create() {
  auto area = std::make_unique<InputArea>();
  area->setOnClick([this](const InputArea::PointerData& /*data*/) {
    requestPanelToggle("control-center", "bluetooth");
  });

  area->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = "bluetooth",
          .glyphSize = Style::barGlyphSize * m_contentScale,
          .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
      })
  );

  if (m_showLabel) {
    area->addChild(
        ui::label({
            .out = &m_label,
            .fontSize = Style::fontSizeBody * m_contentScale,
            .fontWeight = labelFontWeight(),
        })
    );
  }

  setRoot(std::move(area));
}

void BluetoothWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  auto* rootNode = root();
  if (m_glyph == nullptr || rootNode == nullptr) {
    return;
  }
  syncState(renderer);

  m_glyph->measure(renderer);

  float totalWidth = m_glyph->width();
  float contentHeight = m_glyph->height();
  if (m_label != nullptr) {
    m_label->measure(renderer);
    if (m_label->width() > 0.0f) {
      contentHeight = std::max(contentHeight, m_label->height());
      m_label->setPosition(m_glyph->width() + Style::spaceXs, std::round((contentHeight - m_label->height()) * 0.5f));
      totalWidth = m_label->x() + m_label->width();
    }
  }
  m_glyph->setPosition(0.0f, std::round((contentHeight - m_glyph->height()) * 0.5f));
  rootNode->setSize(totalWidth, contentHeight);
}

void BluetoothWidget::doUpdate(Renderer& renderer) { syncState(renderer); }

void BluetoothWidget::syncWidgetVisibility(bool showWidget) {
  if (Node* rootNode = root(); rootNode != nullptr) {
    if (rootNode->visible() != showWidget || rootNode->participatesInLayout() != showWidget) {
      rootNode->setVisible(showWidget);
      rootNode->setParticipatesInLayout(showWidget);
      requestUpdate();
    }
  }
}

void BluetoothWidget::syncState(Renderer& renderer) {
  if (m_glyph == nullptr || m_bluetooth == nullptr) {
    return;
  }

  const auto& s = m_bluetooth->state();
  const auto& devices = m_bluetooth->devices();
  const int numConnected = connectedCount(devices);
  const std::string alias = m_showLabel ? firstConnectedAlias(devices) : std::string{};

  if (m_haveLastState && s == m_lastState && numConnected == m_lastConnectedCount && alias == m_lastConnectedAlias) {
    return;
  }
  m_lastState = s;
  m_haveLastState = true;
  m_lastConnectedCount = numConnected;
  m_lastConnectedAlias = alias;

  const bool hasConnectedDevice = numConnected > 0;
  const bool showWidget = s.adapterPresent && (!m_hideWhenNoConnectedDevice || hasConnectedDevice);
  syncWidgetVisibility(showWidget);
  if (!showWidget) {
    if (Node* rootNode = root(); rootNode != nullptr) {
      rootNode->setOpacity(1.0f);
      if (s.adapterPresent) {
        static_cast<InputArea*>(rootNode)->clearTooltip();
      }
    }
    return;
  }

  auto* rootNode = root();

  if (rootNode != nullptr) {
    rootNode->setOpacity(s.powered ? 1.0f : 0.55f);
  }

  m_glyph->setGlyph(glyphForState(s, numConnected));
  m_glyph->setGlyphSize(Style::barGlyphSize * m_contentScale);
  m_glyph->setColor(
      s.powered ? widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface))
                : colorSpecFromRole(ColorRole::OnSurfaceVariant)
  );
  m_glyph->measure(renderer);

  if (m_label != nullptr) {
    m_label->setText(alias);
    m_label->setColor(
        s.powered ? widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface))
                  : colorSpecFromRole(ColorRole::OnSurfaceVariant)
    );
    m_label->measure(renderer);
  }

  if (rootNode != nullptr) {
    if (numConnected > 0) {
      std::vector<TooltipRow> rows;
      for (const auto& d : devices) {
        if (d.connected) {
          std::string value = d.hasBattery ? std::to_string(d.batteryPercent) + "%" : "Connected";
          rows.push_back({d.alias, std::move(value)});
        }
      }
      static_cast<InputArea*>(rootNode)->setTooltip(std::move(rows));
    } else {
      static_cast<InputArea*>(rootNode)->clearTooltip();
    }
  }

  requestRedraw();
}
