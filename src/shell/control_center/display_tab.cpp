#include "shell/control_center/display_tab.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "system/brightness_service.h"
#include "ui/builders.h"
#include "ui/palette.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

using namespace control_center;

namespace {

  constexpr float kBrightnessSyncEpsilon = 0.005f;
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
    const int width = display.logicalWidth > 0 ? display.logicalWidth : display.physicalWidth;
    const int height = display.logicalHeight > 0 ? display.logicalHeight : display.physicalHeight;
    std::string resolutionText = i18n::tr("control-center.display.unknown-resolution");
    if (width > 0 && height > 0) {
      resolutionText = std::to_string(width) + "x" + std::to_string(height);
    }

    const int physicalWidth = display.physicalWidth;
    const int physicalHeight = display.physicalHeight;
    if (physicalWidth <= 0 || physicalHeight <= 0 || width <= 0 || height <= 0) {
      return resolutionText;
    }

    const double scaleX = static_cast<double>(physicalWidth) / static_cast<double>(width);
    const double scaleY = static_cast<double>(physicalHeight) / static_cast<double>(height);
    const double scale = std::max(0.01, (scaleX + scaleY) * 0.5);
    const int scalePercent = static_cast<int>(std::lround(scale * 100.0));
    return resolutionText + " @ " + std::to_string(scalePercent) + "%";
  }

  std::string formatBrightnessValue(const BrightnessDisplay& display, float brightness) {
    if (!display.controllable) {
      return i18n::tr("control-center.display.disabled");
    }
    return std::to_string(static_cast<int>(std::round(brightness * 100.0f))) + "%";
  }

} // namespace

DisplayTab::DisplayTab(BrightnessService* brightness, ConfigService* /*config*/) : m_brightness(brightness) {}

std::unique_ptr<Flex> DisplayTab::create() {
  const float scale = contentScale();

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
  });

  // Empty state (shown when no displays are known)
  auto emptyState = ui::column(
      {.out = &m_emptyState, .align = FlexAlign::Center, .justify = FlexJustify::Center, .flexGrow = 1.0f},
      ui::label({
          .text = i18n::tr("control-center.display.no-displays"),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  tab->addChild(std::move(emptyState));

  return tab;
}

void DisplayTab::setActive(bool active) {
  if (active && m_brightness != nullptr) {
    m_brightness->requestDdcRefresh();
  }
}

void DisplayTab::onClose() {
  flushPendingBrightness(true);
  m_debounceTimer.stop();
  m_rootLayout = nullptr;
  m_emptyState = nullptr;
  m_cards.clear();
  m_lastDisplayListKey.clear();
}

bool DisplayTab::dragging() const noexcept {
  for (const auto& card : m_cards) {
    if (card.slider != nullptr && card.slider->dragging()) {
      return true;
    }
  }
  return false;
}

void DisplayTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_rootLayout == nullptr) {
    return;
  }

  rebuildCards(renderer);

  const float scale = contentScale();
  const float cardWidth = std::max(1.0f, contentWidth);
  const float cardInnerWidth = std::max(1.0f, cardWidth - Style::spaceMd * scale * 2.0f);
  const float headerTextMaxWidth =
      std::max(1.0f, cardInnerWidth - Style::fontSizeTitle * scale - Style::spaceSm * scale);
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

  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
}

void DisplayTab::doUpdate(Renderer& renderer) {
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
    const bool isPending = m_pendingDisplayId == card.displayId && m_pendingBrightness >= 0.0f;
    const bool holdState = isDragging && m_lastSentBrightness >= 0.0f && now < m_ignoreStateUntil &&
                           std::abs(display->brightness - m_lastSentBrightness) > 0.02f;

    const float displayedBrightness = std::clamp(
        isPending ? m_pendingBrightness : (holdState ? m_lastSentBrightness : display->brightness), 0.0f, 1.0f
    );

    if (!isDragging &&
        (!card.lastControllable || std::abs(displayedBrightness - card.lastBrightness) >= kBrightnessSyncEpsilon)) {
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

void DisplayTab::rebuildCards(Renderer& /*renderer*/) {
  if (m_brightness == nullptr || m_rootLayout == nullptr) {
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
      m_rootLayout->removeChild(card.card);
    }
  }
  m_cards.clear();

  // Show/hide empty state
  const bool empty = displays.empty();
  if (m_emptyState != nullptr) {
    m_emptyState->setVisible(empty);
  }

  if (empty) {
    return;
  }

  const float scale = contentScale();

  for (const auto& display : displays) {
    // Card container
    auto card = ui::column({
        .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& section) {
          applySectionCardStyle(section, scale, opacity, borders);
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
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .fontWeight = FontWeight::Bold,
            .flexGrow = 1.0f,
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
    Slider* sliderPtr = nullptr;
    auto slider = ui::slider({
        .out = &sliderPtr,
        .minValue = 0.0f,
        .maxValue = 1.0f,
        .step = 0.01f,
        .value = display.brightness,
        .enabled = display.controllable,
        .trackHeight = Style::sliderTrackHeight * scale,
        .thumbSize = Style::sliderThumbSize * scale,
        .controlHeight = Style::controlHeight * scale,
        .flexGrow = 1.0f,
        .onValueChanged =
            [this, displayId](float value) {
              if (m_syncingSlider) {
                return;
              }
              const auto* currentDisplay = m_brightness != nullptr ? m_brightness->findDisplay(displayId) : nullptr;
              if (currentDisplay == nullptr || !currentDisplay->controllable) {
                return;
              }
              queueBrightness(displayId, value);
              // Update the value label immediately
              for (auto& c : m_cards) {
                if (c.displayId == displayId && c.valueLabel != nullptr) {
                  c.valueLabel->setText(formatBrightnessValue(*currentDisplay, value));
                  c.lastBrightness = value;
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
    m_rootLayout->addChild(std::move(card));

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

void DisplayTab::queueBrightness(const std::string& displayId, float value) {
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

void DisplayTab::flushPendingBrightness(bool /*force*/) {
  m_debounceTimer.stop();

  if (m_pendingBrightness < 0.0f || m_brightness == nullptr) {
    return;
  }

  const auto* display = m_brightness->findDisplay(m_pendingDisplayId);
  if (display == nullptr || !display->controllable) {
    m_pendingBrightness = -1.0f;
    return;
  }

  m_brightness->setBrightness(m_pendingDisplayId, m_pendingBrightness);
  m_lastSentBrightness = m_pendingBrightness;
  m_lastCommitAt = std::chrono::steady_clock::now();
  m_ignoreStateUntil = m_lastCommitAt + kBrightnessStateHoldoff;
  m_pendingBrightness = -1.0f;
}
