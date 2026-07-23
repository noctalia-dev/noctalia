#include "shell/bar/widgets/battery_widget.h"

#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "render/scene/input_area.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <sstream>
#include <string>
#include <utility>

namespace {

  constexpr float kGraphicBodyWidth = 22.0f;
  constexpr float kGraphicBodyHeight = 14.0f;
  constexpr float kGraphicTerminalWidth = 2.5f;
  constexpr float kGraphicTerminalHeight = 7.0f;
  constexpr float kGraphicCornerRadius = 3.0f;

  ColorSpec withOpacity(ColorSpec color, float opacity) {
    color.alpha *= opacity;
    return color;
  }

  const char* batteryStateGlyph(BatteryState state) {
    if (state == BatteryState::Charging) {
      return "bolt-filled";
    }
    if (state == BatteryState::FullyCharged || state == BatteryState::PendingCharge) {
      return "plug-filled";
    }
    return nullptr;
  }

  std::string formatCompactDuration(std::int64_t seconds) {
    const auto hours = seconds / 3600;
    const auto minutes = (seconds % 3600) / 60;
    const std::string hourText = i18n::tr("time.units.hour-compact", "count", hours);
    const std::string minuteText = i18n::tr("time.units.minute-compact", "count", minutes);
    if (hours > 0 && minutes > 0) {
      return i18n::tr("time.duration.two-parts", "first", hourText, "second", minuteText);
    }
    if (hours > 0) {
      return hourText;
    }
    if (minutes > 0) {
      return minuteText;
    }
    return i18n::tr("time.duration.less-than-minute");
  }

} // namespace

BatteryWidget::BatteryWidget(UPowerService* upower, Options options)
    : m_upower(upower), m_deviceSelector(std::move(options.deviceSelector)),
      m_warningThreshold(options.warningThreshold), m_warningColor(options.warningColor),
      m_displayMode(options.displayMode), m_labelContent(options.labelContent), m_showLabel(options.showLabel),
      m_hideWhenPlugged(options.hideWhenPlugged), m_hideWhenFull(options.hideWhenFull) {}

// Vertical bars are too narrow for time or rate text, so they always show the bare percentage; the
// tooltip carries the full detail. Time and rate are only known while the battery is actively charging
// or discharging, and the percentage stands in whenever the selected content has no value to show.
std::string BatteryWidget::buildLabelText(int pct, const UPowerState& state) const {
  if (m_isVertical) {
    return std::to_string(pct);
  }

  switch (m_labelContent) {
  case BatteryLabelContent::Time:
    if (state.state == BatteryState::Discharging && state.timeToEmpty > 0) {
      return formatCompactDuration(state.timeToEmpty);
    }
    if (state.state == BatteryState::Charging && state.timeToFull > 0) {
      return formatCompactDuration(state.timeToFull);
    }
    break;
  case BatteryLabelContent::Rate:
    if (state.energyRate > 0.0) {
      return std::format("{:.1f} W", state.energyRate);
    }
    break;
  case BatteryLabelContent::Percent:
    break;
  }
  return std::format("{}%", pct);
}

void BatteryWidget::create() {
  auto container = std::make_unique<InputArea>();
  container->setOnClick([this](const InputArea::PointerData& /*data*/) {
    requestPanelToggle("control-center", "power");
  });
  setRoot(std::move(container));

  if (m_displayMode == BatteryDisplayMode::Graphic) {
    createGraphicMode();
  } else if (m_displayMode == BatteryDisplayMode::Glyph) {
    createGlyphMode();
  } else {
    createLabelOnlyMode();
  }
}

void BatteryWidget::createGraphicMode() {
  auto* container = static_cast<InputArea*>(root());

  container->addChild(
      ui::box({
          .out = &m_bodyBg,
          .fill = withOpacity(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)), 0.25f),
      })
  );

  container->addChild(
      ui::box({
          .out = &m_fillRect,
      })
  );

  container->addChild(
      ui::box({
          .out = &m_terminalNub,
          .fill = withOpacity(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)), 0.25f),
      })
  );

  if (m_showLabel) {
    container->addChild(
        ui::label({
            .out = &m_overlayLabel,
            .fontWeight = labelFontWeight(),
            .fontFamily = labelFontFamily(),
            .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
        })
    );
  }

  container->addChild(
      ui::glyph({
          .out = &m_overlayGlyph,
          .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
          .visible = false,
      })
  );
}

void BatteryWidget::createGlyphMode() {
  auto* container = static_cast<InputArea*>(root());

  container->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = "battery-4",
          .glyphSize = Style::baseGlyphSize * m_contentScale,
          .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
      })
  );

  container->addChild(
      ui::label({
          .out = &m_label,
          .fontSize = Style::fontSizeBody * m_contentScale,
          .fontWeight = labelFontWeight(),
          .fontFamily = labelFontFamily(),
          .visible = m_showLabel,
      })
  );
}

void BatteryWidget::createLabelOnlyMode() {
  auto* container = static_cast<InputArea*>(root());

  container->addChild(
      ui::label({
          .out = &m_label,
          .fontSize = Style::fontSizeBody * m_contentScale,
          .fontWeight = labelFontWeight(),
          .fontFamily = labelFontFamily(),
          .visible = m_showLabel,
      })
  );
}

void BatteryWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  auto* rootNode = root();
  if (rootNode == nullptr) {
    return;
  }
  m_isVertical = containerHeight > containerWidth;
  syncState(renderer);

  if (m_displayMode == BatteryDisplayMode::Graphic) {
    layoutGraphicMode(renderer);
  } else if (m_displayMode == BatteryDisplayMode::Glyph) {
    layoutGlyphMode(renderer, containerWidth, containerHeight);
  } else {
    layoutLabelOnlyMode(renderer, containerWidth, containerHeight);
  }
}

void BatteryWidget::layoutGraphicMode(Renderer& renderer) {
  auto* rootNode = root();
  if (m_bodyBg == nullptr || m_fillRect == nullptr || m_terminalNub == nullptr || rootNode == nullptr) {
    return;
  }

  const float scale = (Style::fontSizeBody / 14.0f) * m_contentScale;
  const float bodyW = std::round(kGraphicBodyWidth * scale);
  const float bodyH = std::round(kGraphicBodyHeight * scale);
  const float termW = std::round(kGraphicTerminalWidth * scale);
  const float termH = std::round(kGraphicTerminalHeight * scale);
  const float cornerR = std::round(kGraphicCornerRadius * scale);
  const float labelGap = Style::spaceXs * m_contentScale;
  const float stateGap = std::round(Style::spaceXs * 0.5f * m_contentScale);
  const bool showLabel = m_overlayLabel != nullptr && m_showLabel;
  const bool showStateGlyph = m_overlayGlyph != nullptr && m_overlayGlyph->visible();
  const bool showStateGlyphOutside = showStateGlyph && showLabel;
  const bool showStateGlyphInside = showStateGlyph && !showLabel;
  if (showLabel) {
    m_overlayLabel->setFontSize((m_isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * m_contentScale);
    m_overlayLabel->measure(renderer);
  }
  if (showStateGlyph) {
    m_overlayGlyph->setGlyphSize(Style::fontSizeCaption * m_contentScale);
    m_overlayGlyph->measure(renderer);
  }

  if (m_isVertical) {
    const float graphicW = bodyH;
    const float graphicH = bodyW + termW;
    const float labelW = showLabel ? m_overlayLabel->width() : 0.0f;
    const float labelH = showLabel ? m_overlayLabel->height() : 0.0f;
    const float stateW = showStateGlyphOutside ? m_overlayGlyph->width() : 0.0f;
    const float stateH = showStateGlyphOutside ? m_overlayGlyph->height() : 0.0f;
    const float labelGroupH = labelH + (showStateGlyphOutside ? stateGap + stateH : 0.0f);
    const float rootW = std::max({graphicW, labelW, stateW});
    const float bodyX = std::round((rootW - graphicW) * 0.5f);
    const float bodyY = termW;

    m_bodyBg->setRadius(cornerR);
    m_bodyBg->setPosition(bodyX, bodyY);
    m_bodyBg->setSize(bodyH, bodyW);

    m_terminalNub->setRadius(cornerR * 0.5f);
    m_terminalNub->setPosition(bodyX + std::round((bodyH - termH) * 0.5f), 0.0f);
    m_terminalNub->setSize(termH, termW);

    m_fillRect->setRadius(cornerR);
    updateFillGeometry();

    if (showLabel) {
      m_overlayLabel->setPosition(std::round((rootW - labelW) * 0.5f), graphicH + labelGap);
    }

    if (showStateGlyphOutside) {
      m_overlayGlyph->setPosition(std::round((rootW - stateW) * 0.5f), graphicH + labelGap + labelH + stateGap);
    } else if (showStateGlyphInside) {
      m_overlayGlyph->setPosition(
          bodyX + std::round((bodyH - m_overlayGlyph->width()) * 0.5f),
          bodyY + std::round((bodyW - m_overlayGlyph->height()) * 0.5f)
      );
    }

    rootNode->setSize(rootW, graphicH + (showLabel ? labelGap + labelGroupH : 0.0f));
  } else {
    const float graphicW = bodyW + termW;
    const float graphicH = bodyH;
    const float labelW = showLabel ? m_overlayLabel->width() : 0.0f;
    const float labelH = showLabel ? m_overlayLabel->height() : 0.0f;
    const float stateW = showStateGlyphOutside ? m_overlayGlyph->width() : 0.0f;
    const float stateH = showStateGlyphOutside ? m_overlayGlyph->height() : 0.0f;
    const float labelGroupW = labelW + (showStateGlyphOutside ? stateGap + stateW : 0.0f);
    const float labelGroupH = std::max(labelH, stateH);
    const float rootH = std::max(graphicH, labelGroupH);
    const float bodyY = std::round((rootH - bodyH) * 0.5f);

    m_bodyBg->setRadius(cornerR);
    m_bodyBg->setPosition(0.0f, bodyY);
    m_bodyBg->setSize(bodyW, bodyH);

    m_terminalNub->setRadius(cornerR * 0.5f);
    m_terminalNub->setPosition(bodyW, bodyY + std::round((bodyH - termH) * 0.5f));
    m_terminalNub->setSize(termW, termH);

    m_fillRect->setRadius(cornerR);
    updateFillGeometry();

    if (showLabel) {
      m_overlayLabel->setPosition(graphicW + labelGap, std::round((rootH - labelH) * 0.5f));
    }

    if (showStateGlyphOutside) {
      m_overlayGlyph->setPosition(graphicW + labelGap + labelW + stateGap, std::round((rootH - stateH) * 0.5f));
    } else if (showStateGlyphInside) {
      m_overlayGlyph->setPosition(
          std::round((bodyW - m_overlayGlyph->width()) * 0.5f),
          bodyY + std::round((bodyH - m_overlayGlyph->height()) * 0.5f)
      );
    }

    rootNode->setSize(graphicW + (showLabel ? labelGap + labelGroupW : 0.0f), rootH);
  }
}

void BatteryWidget::layoutGlyphMode(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  auto* rootNode = root();
  if (m_glyph == nullptr || rootNode == nullptr) {
    return;
  }

  m_glyph->measure(renderer);

  if (m_label != nullptr && m_showLabel) {
    m_label->measure(renderer);

    if (m_isVertical) {
      const float w = std::max(m_glyph->width(), m_label->width());
      m_glyph->setPosition(std::round((w - m_glyph->width()) * 0.5f), 0.0f);
      m_label->setPosition(std::round((w - m_label->width()) * 0.5f), m_glyph->height());
      rootNode->setSize(w, m_glyph->height() + m_label->height());
    } else {
      const float h = std::max(m_glyph->height(), m_label->height());
      m_glyph->setPosition(0.0f, std::round((h - m_glyph->height()) * 0.5f));
      m_label->setPosition(m_glyph->width() + Style::spaceXs, std::round((h - m_label->height()) * 0.5f));
      rootNode->setSize(m_label->x() + m_label->width(), h);
    }
  } else {
    rootNode->setSize(m_glyph->width(), m_glyph->height());
  }
}

void BatteryWidget::layoutLabelOnlyMode(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  auto* rootNode = root();
  if (m_label == nullptr || rootNode == nullptr) {
    return;
  }

  m_label->setFontSize((m_isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * m_contentScale);
  m_label->measure(renderer);
  rootNode->setSize(m_label->width(), m_label->height());
}

void BatteryWidget::updateFillGeometry() {
  if (m_fillRect == nullptr || m_bodyBg == nullptr) {
    return;
  }

  const float fraction = std::clamp(m_animatedPct / 100.0f, 0.0f, 1.0f);

  if (m_isVertical) {
    const float bodyW = m_bodyBg->width();
    const float bodyH = m_bodyBg->height();
    const float fillH = bodyH * fraction;
    m_fillRect->setPosition(m_bodyBg->x(), m_bodyBg->y() + bodyH - fillH);
    m_fillRect->setSize(bodyW, fillH);
  } else {
    const float bodyW = m_bodyBg->width();
    const float bodyH = m_bodyBg->height();
    const float fillW = bodyW * fraction;
    m_fillRect->setPosition(m_bodyBg->x(), m_bodyBg->y());
    m_fillRect->setSize(fillW, bodyH);
  }
}

void BatteryWidget::doUpdate(Renderer& renderer) { syncState(renderer); }

void BatteryWidget::onFrameTick(float /*deltaMs*/) { requestRedraw(); }

bool BatteryWidget::needsFrameTick() const { return m_displayMode == BatteryDisplayMode::Graphic && m_fillAnim != 0; }

void BatteryWidget::syncState(Renderer& renderer) {
  if (m_upower == nullptr) {
    return;
  }

  const auto s = m_upower->stateForDevice(m_deviceSelector);

  const auto now = std::chrono::steady_clock::now();
  const bool forceTimeRefresh = (m_lastTooltipRefreshTime == std::chrono::steady_clock::time_point{})
      || (now - m_lastTooltipRefreshTime >= std::chrono::seconds(15));

  if (s.percentage == m_lastPct
      && s.state == m_lastState
      && s.isPresent == m_lastPresent
      && s.energyRate == m_lastEnergyRate
      && s.timeToEmpty == m_lastTimeToEmpty
      && m_isVertical == m_lastVertical
      && !forceTimeRefresh) {
    return;
  }

  m_lastPct = s.percentage;
  m_lastState = s.state;
  m_lastPresent = s.isPresent;
  m_lastEnergyRate = s.energyRate;
  m_lastTimeToEmpty = s.timeToEmpty;
  m_lastVertical = m_isVertical;
  m_lastTooltipRefreshTime = now;

  const bool isPluggedIn = s.state == BatteryState::Charging
      || s.state == BatteryState::FullyCharged
      || s.state == BatteryState::PendingCharge;

  const bool hasVisibleContent = m_displayMode != BatteryDisplayMode::None || m_showLabel;

  const bool showWidget = s.isPresent
      && hasVisibleContent
      && !(m_hideWhenPlugged && isPluggedIn)
      && !(m_hideWhenFull && (s.state == BatteryState::FullyCharged || s.state == BatteryState::PendingCharge));

  auto* rootNode = root();
  if (rootNode != nullptr) {
    rootNode->setVisible(showWidget);
    rootNode->setParticipatesInLayout(showWidget);
  }

  if (!showWidget) {
    return;
  }

  const int pct = static_cast<int>(std::round(s.percentage));
  const bool isWarning = m_warningThreshold > 0 && pct <= m_warningThreshold && !isPluggedIn;

  if (m_displayMode == BatteryDisplayMode::Graphic) {
    const ColorSpec normalFgColor = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface));
    const ColorSpec fgColor = isWarning ? m_warningColor : normalFgColor;

    if (m_fillRect != nullptr) {
      m_fillRect->setFill(fgColor);
    }
    if (m_bodyBg != nullptr) {
      m_bodyBg->setFill(withOpacity(fgColor, 0.25f));
    }

    if (m_terminalNub != nullptr) {
      m_terminalNub->setFill(isWarning ? m_warningColor : withOpacity(normalFgColor, 0.25f));
    }

    // Animate fill percentage
    const auto newPct = static_cast<float>(s.percentage);
    if (m_animations != nullptr && std::abs(m_animatedPct - newPct) > 0.5f) {
      m_animations->cancel(m_fillAnim);
      m_fillAnim = m_animations->animate(
          m_animatedPct, newPct, static_cast<float>(Style::animNormal), Easing::EaseOutCubic,
          [this](float v) {
            m_animatedPct = v;
            updateFillGeometry();
            requestRedraw();
          },
          [this]() { m_fillAnim = 0; }, this
      );
      requestFrameTick();
    } else {
      m_animatedPct = newPct;
      updateFillGeometry();
    }

    // Graphic mode label
    if (m_overlayLabel != nullptr && m_showLabel) {
      m_overlayLabel->setText(buildLabelText(pct, s));
      m_overlayLabel->setColor(fgColor);
      m_overlayLabel->measure(renderer);
    }

    // Overlay glyph — state icon
    const char* stateGlyph = batteryStateGlyph(s.state);
    if (m_overlayGlyph != nullptr) {
      if (stateGlyph != nullptr) {
        m_overlayGlyph->setGlyph(stateGlyph);
        m_overlayGlyph->setColor(fgColor);
        m_overlayGlyph->measure(renderer);
      }
    }

    if (m_overlayLabel != nullptr) {
      m_overlayLabel->setVisible(m_showLabel);
    }
    if (m_overlayGlyph != nullptr) {
      m_overlayGlyph->setVisible(stateGlyph != nullptr);
    }
  } else if (m_displayMode == BatteryDisplayMode::Glyph) {
    const ColorSpec normalFgColor = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface));
    const ColorSpec fgColor = isWarning ? m_warningColor : normalFgColor;

    if (m_glyph != nullptr) {
      m_glyph->setGlyph(batteryGlyphName(s.percentage, s.state));
      m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
      m_glyph->setColor(fgColor);
      m_glyph->measure(renderer);
    }

    if (m_label != nullptr && m_showLabel) {
      m_label->setFontSize((m_isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * m_contentScale);
      m_label->setText(buildLabelText(pct, s));
      m_label->setColor(fgColor);
      m_label->measure(renderer);
    }
  } else if (m_displayMode == BatteryDisplayMode::None) {
    const ColorSpec normalFgColor = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface));
    const ColorSpec fgColor = isWarning ? m_warningColor : normalFgColor;

    if (m_label != nullptr && m_showLabel) {
      m_label->setFontSize((m_isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * m_contentScale);
      m_label->setText(buildLabelText(pct, s));
      m_label->setColor(fgColor);
      m_label->measure(renderer);
    }
  }

  // Tooltip (both modes)
  if (rootNode != nullptr) {
    auto devices = m_upower->batteryDevices();
    auto laptopEnd =
        std::ranges::stable_partition(devices, [](const UPowerDeviceInfo& d) { return d.isLaptopBattery(); }).begin();
    int laptopBatteryCount = static_cast<int>(laptopEnd - devices.begin());

    std::vector<TooltipRow> rows;
    int laptopBatteryIndex = 0;
    for (const auto& dev : devices) {
      std::string name;
      if (dev.isLaptopBattery()) {
        name = (laptopBatteryCount > 1)
            ? i18n::tr("power.battery.tooltip.device-numbered", "index", ++laptopBatteryIndex)
            : i18n::tr("power.battery.tooltip.device");
      } else {
        name = !dev.model.empty()
            ? dev.model
            : (!dev.nativePath.empty() ? dev.nativePath : i18n::tr("power.battery.tooltip.unknown-device"));
      }
      int dp = static_cast<int>(std::round(dev.state.percentage));
      rows.push_back({std::move(name), std::to_string(dp) + "%"});

      if (dev.isLaptopBattery()) {
        rows.push_back({i18n::tr("power.battery.tooltip.status"), batteryStateLabel(dev.state.state)});

        if (dev.state.timeToEmpty > 0) {
          auto dur = formatDuration(std::chrono::seconds(dev.state.timeToEmpty));
          rows.push_back({i18n::tr("power.battery.tooltip.time-left"), std::move(dur)});
        } else if (dev.state.timeToFull > 0) {
          auto dur = formatDuration(std::chrono::seconds(dev.state.timeToFull));
          rows.push_back({i18n::tr("power.battery.tooltip.time-to-full"), std::move(dur)});
        }

        if (dev.state.energyRate > 0.0) {
          std::ostringstream oss;
          oss << std::fixed;
          oss.precision(1);
          oss << dev.state.energyRate << " W";
          rows.push_back({i18n::tr("power.battery.tooltip.rate"), oss.str()});
        }

        if (dev.energyFullDesign > 0.0) {
          int health = static_cast<int>(std::round(dev.energyFull / dev.energyFullDesign * 100.0));
          rows.push_back({i18n::tr("power.battery.tooltip.health"), std::to_string(health) + "%"});
        }
      }
    }
    if (!rows.empty()) {
      static_cast<InputArea*>(rootNode)->setTooltip(std::move(rows));
    } else {
      static_cast<InputArea*>(rootNode)->clearTooltip();
    }
  }

  requestRedraw();
}
