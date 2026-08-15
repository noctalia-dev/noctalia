#include "shell/control_center/tabs/monitor_tab.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "system/brightness_service.h"
#include "ui/builders.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

using namespace control_center;

namespace {

  constexpr float kBrightnessSyncEpsilon = 0.005F;
  constexpr auto kBrightnessCommitInterval = std::chrono::milliseconds(16);
  constexpr auto kBrightnessStateHoldoff = std::chrono::milliseconds(180);

  std::string buildDisplayListKey(const std::vector<BrightnessDisplay>& displays) {
    std::string key;
    for (const auto& d : displays) {
      key += d.id;
      key += d.controllable ? ":1" : ":0";
      key += ';';
    }
    return key;
  }

  std::string formatDisplayInfo(const BrightnessDisplay& display) {
    // Show the native hardware resolution; the fractionally-scaled logical size confuses
    // users who expect the panel's real pixel count. The scale is surfaced separately below.
    const int physicalWidth = display.physicalWidth;
    const int physicalHeight = display.physicalHeight;
    const int width = physicalWidth > 0 ? physicalWidth : display.logicalWidth;
    const int height = physicalHeight > 0 ? physicalHeight : display.logicalHeight;
    std::string resolutionText = i18n::tr("control-center.display.unknown-resolution");
    if (width > 0 && height > 0) {
      resolutionText = std::to_string(width) + "x" + std::to_string(height);
    }

    const int logicalWidth = display.logicalWidth;
    const int logicalHeight = display.logicalHeight;
    if (physicalWidth <= 0 || physicalHeight <= 0 || logicalWidth <= 0 || logicalHeight <= 0) {
      return resolutionText;
    }

    const double scaleX = static_cast<double>(physicalWidth) / static_cast<double>(logicalWidth);
    const double scaleY = static_cast<double>(physicalHeight) / static_cast<double>(logicalHeight);
    const double scale = std::max(0.01, (scaleX + scaleY) * 0.5);
    const int scalePercent = static_cast<int>(std::lround(scale * 100.0));
    return resolutionText + " @ " + std::to_string(scalePercent) + "%";
  }

  std::string formatBrightnessValue(const BrightnessDisplay& display, float brightness) {
    if (!display.controllable) {
      return i18n::tr("control-center.display.disabled");
    }
    return std::to_string(static_cast<int>(std::round(brightness * 100.0F))) + "%";
  }

} // namespace

MonitorTab::MonitorTab(BrightnessService* brightness, ConfigService* config)
    : m_brightness(brightness), m_configService(config) {}

std::unique_ptr<Flex> MonitorTab::create() {
  const float scale = contentScale();

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
  });

  auto cardsScroll = ui::scrollView({
      .out = &m_cardsScroll,
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0F,
      .viewportPaddingV = 0.0F,
      .flexGrow = 1.0F,
      .configure = [](ScrollView& scrollView) {
        scrollView.clearFill();
        scrollView.clearBorder();
      },
  });
  m_cardsLayout = cardsScroll->content();
  m_cardsLayout->setDirection(FlexDirection::Vertical);
  m_cardsLayout->setAlign(FlexAlign::Stretch);
  m_cardsLayout->setGap(Style::spaceMd * scale);
  tab->addChild(std::move(cardsScroll));

  // Empty state (shown when no displays are known)
  auto emptyState = ui::column(
      {.out = &m_emptyState,
       .align = FlexAlign::Center,
       .justify = FlexJustify::Center,
       .flexGrow = 1.0F,
       .visible = false},
      ui::label({
          .text = i18n::tr("control-center.display.no-displays"),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  tab->addChild(std::move(emptyState));

  return tab;
}

void MonitorTab::setActive(bool active) {
  const bool becameActive = active && !m_active;
  m_active = active;
  if (becameActive && m_brightness != nullptr) {
    m_brightness->requestDdcRefresh();
  }
}

void MonitorTab::onClose() {
  flushPendingBrightness(true);
  m_debounceTimer.stop();
  m_rootLayout = nullptr;
  m_emptyState = nullptr;
  m_cardsScroll = nullptr;
  m_cardsLayout = nullptr;
  m_cards.clear();
  m_lastDisplayListKey.clear();
}

bool MonitorTab::dragging() const noexcept {
  for (const auto& card : m_cards) {
    if (card.slider != nullptr && card.slider->dragging()) {
      return true;
    }
  }
  return false;
}

void MonitorTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_rootLayout == nullptr) {
    return;
  }

  rebuildCards(renderer);

  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);

  const float scale = contentScale();
  const float cardWidth =
      std::max(1.0F, m_cardsScroll != nullptr ? m_cardsScroll->contentViewportWidth() : contentWidth);
  const float cardInnerWidth = std::max(1.0F, cardWidth - Style::spaceMd * scale * 2.0F);
  const float headerTextMaxWidth =
      std::max(1.0F, cardInnerWidth - Style::fontSizeTitle * scale - Style::spaceSm * scale);
  for (auto& card : m_cards) {
    if (card.card != nullptr) {
      card.card->setMinWidth(cardWidth);
    }
    if (card.nameLabel != nullptr) {
      card.nameLabel->setMaxLines(1);
      card.nameLabel->setMaxWidth(headerTextMaxWidth);
    }
    if (card.detailsLabel != nullptr) {
      card.detailsLabel->setMaxLines(1);
      card.detailsLabel->setMaxWidth(cardInnerWidth);
    }
  }

  m_rootLayout->layout(renderer);
}

void MonitorTab::doUpdate(Renderer& renderer) {
  rebuildCards(renderer);

  if (m_brightness == nullptr) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();

  for (auto& card : m_cards) {
    const auto* display = m_brightness->findDisplay(card.displayId);
    if (display == nullptr || card.slider == nullptr) {
      continue;
    }

    card.slider->setEnabled(display->controllable);

    const std::string infoText = formatDisplayInfo(*display);
    if (card.detailsLabel != nullptr && card.lastDisplayInfo != infoText) {
      card.detailsLabel->setText(infoText);
      card.lastDisplayInfo = infoText;
    }

    if (!display->controllable) {
      if (card.valueLabel != nullptr) {
        card.valueLabel->setText(formatBrightnessValue(*display, display->brightness));
      }
      if (!card.slider->dragging() && std::abs(display->brightness - card.lastBrightness) >= kBrightnessSyncEpsilon) {
        m_syncingSlider = true;
        card.slider->setValue(display->brightness);
        m_syncingSlider = false;
        card.lastBrightness = display->brightness;
      }
      card.lastControllable = false;
      continue;
    }

    const bool isDragging = card.slider->dragging();
    const bool isPending = m_pendingDisplayId == card.displayId && m_pendingBrightness >= 0.0F;
    const bool holdState = isDragging
        && m_lastSentBrightness >= 0.0F
        && now < m_ignoreStateUntil
        && std::abs(display->brightness - m_lastSentBrightness) > 0.02F;

    const float minBrightness = m_configService->config().brightness.minimumBrightness;
    const float displayedBrightness = std::clamp(
        isPending ? m_pendingBrightness : (holdState ? m_lastSentBrightness : display->brightness), minBrightness, 1.0F
    );

    if (!isDragging
        && (!card.lastControllable || std::abs(displayedBrightness - card.lastBrightness) >= kBrightnessSyncEpsilon)) {
      m_syncingSlider = true;
      card.slider->setValue(displayedBrightness);
      m_syncingSlider = false;
      if (card.valueLabel != nullptr) {
        card.valueLabel->setText(formatBrightnessValue(*display, displayedBrightness));
      }
      card.lastBrightness = displayedBrightness;
    }
    card.lastControllable = true;
  }
}

void MonitorTab::rebuildCards(Renderer& /*renderer*/) {
  if (m_brightness == nullptr || m_cardsLayout == nullptr) {
    return;
  }

  const auto& displays = m_brightness->displays();
  const std::string key = buildDisplayListKey(displays);
  if (key == m_lastDisplayListKey) {
    return;
  }
  m_lastDisplayListKey = key;

  // Remove old cards
  for (auto& card : m_cards) {
    if (card.card != nullptr) {
      m_cardsLayout->removeChild(card.card);
    }
  }
  m_cards.clear();

  // Show/hide empty state
  const bool empty = displays.empty();
  if (m_emptyState != nullptr) {
    m_emptyState->setVisible(empty);
  }
  if (m_cardsScroll != nullptr) {
    m_cardsScroll->setVisible(!empty);
  }

  if (empty) {
    return;
  }

  const float scale = contentScale();

  for (const auto& display : displays) {
    // Card container
    auto card = ui::column({
        .configure = [scale, opacity = panelCardOpacity()](Flex& section) {
          applySectionCardStyle(section, scale, opacity);
          section.setGap(Style::spaceMd * scale);
        },
    });

    Glyph* iconPtr = nullptr;
    Label* nameLabelPtr = nullptr;
    auto headerRow = ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
        ui::glyph({
            .out = &iconPtr,
            .glyph = "device-desktop",
            .glyphSize = Style::fontSizeTitle * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        }),
        ui::label({
            .out = &nameLabelPtr,
            .text = display.label,
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0F,
        })
    );
    card->addChild(std::move(headerRow));

    const std::string infoText = formatDisplayInfo(display);
    Label* detailsLabelPtr = nullptr;
    card->addChild(
        ui::label({
            .out = &detailsLabelPtr,
            .text = infoText,
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );

    // Slider row: sun icon + slider + percentage
    auto sliderRow = ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
        ui::glyph({
            .glyph = "brightness-low",
            .glyphSize = Style::fontSizeTitle * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );

    const std::string displayId = display.id;
    const float minBrightness = m_configService->config().brightness.minimumBrightness;
    Slider* sliderPtr = nullptr;
    auto slider = ui::slider({
        .out = &sliderPtr,
        .minValue = minBrightness,
        .maxValue = 1.0F,
        .step = 0.01F,
        .value = std::max(display.brightness, minBrightness),
        .enabled = display.controllable,
        .trackHeight = Style::sliderTrackHeight * scale,
        .thumbSize = Style::sliderThumbSize * scale,
        .controlHeight = Style::controlHeight * scale,
        .flexGrow = 1.0F,
        .onValueChanged =
            [this, displayId](double value) {
              if (m_syncingSlider) {
                return;
              }
              const auto* currentDisplay = m_brightness != nullptr ? m_brightness->findDisplay(displayId) : nullptr;
              if (currentDisplay == nullptr || !currentDisplay->controllable) {
                return;
              }
              const auto brightness = static_cast<float>(value);
              queueBrightness(displayId, brightness);
              // Update the value label immediately
              for (auto& c : m_cards) {
                if (c.displayId == displayId && c.valueLabel != nullptr) {
                  c.valueLabel->setText(formatBrightnessValue(*currentDisplay, brightness));
                  c.lastBrightness = brightness;
                  break;
                }
              }
            },
        .onDragEnd = [this]() { flushPendingBrightness(true); },
    });
    sliderRow->addChild(std::move(slider));

    Label* valueLabelPtr = nullptr;
    sliderRow->addChild(
        ui::glyph({
            .glyph = "brightness-high",
            .glyphSize = Style::fontSizeTitle * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
    sliderRow->addChild(
        ui::label({
            .out = &valueLabelPtr,
            .text = formatBrightnessValue(display, display.brightness),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .minWidth = Style::controlHeightLg * scale,
        })
    );

    card->addChild(std::move(sliderRow));

    auto* cardPtr = card.get();
    m_cardsLayout->addChild(std::move(card));

    m_cards.push_back(
        DisplayCard{
            .displayId = display.id,
            .card = cardPtr,
            .nameLabel = nameLabelPtr,
            .detailsLabel = detailsLabelPtr,
            .icon = iconPtr,
            .slider = sliderPtr,
            .valueLabel = valueLabelPtr,
            .lastBrightness = display.brightness,
            .lastControllable = display.controllable,
            .lastDisplayInfo = infoText,
        }
    );
  }
}

void MonitorTab::queueBrightness(const std::string& displayId, float value) {
  m_pendingDisplayId = displayId;
  m_pendingBrightness = value;

  const auto now = std::chrono::steady_clock::now();
  if (now - m_lastCommitAt >= kBrightnessCommitInterval) {
    flushPendingBrightness();
    return;
  }

  if (!m_debounceTimer.active()) {
    m_debounceTimer.start(kBrightnessCommitInterval, [this]() { flushPendingBrightness(); });
  }
}

void MonitorTab::flushPendingBrightness(bool /*force*/) {
  m_debounceTimer.stop();

  if (m_pendingBrightness < 0.0F || m_brightness == nullptr) {
    return;
  }

  const auto* display = m_brightness->findDisplay(m_pendingDisplayId);
  if (display == nullptr || !display->controllable) {
    m_pendingBrightness = -1.0F;
    return;
  }

  m_brightness->setBrightness(m_pendingDisplayId, m_pendingBrightness);
  m_lastSentBrightness = m_pendingBrightness;
  m_lastCommitAt = std::chrono::steady_clock::now();
  m_ignoreStateUntil = m_lastCommitAt + kBrightnessStateHoldoff;
  m_pendingBrightness = -1.0F;
}
