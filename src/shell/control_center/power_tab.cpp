#include "shell/control_center/power_tab.h"

#include "dbus/power/power_profiles_service.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/palette.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <string>

using namespace control_center;

namespace {

  ColorRole healthRole(double health) {
    if (health >= 80.0) {
      return ColorRole::Primary;
    }
    if (health >= 50.0) {
      return ColorRole::Tertiary;
    }
    return ColorRole::Error;
  }

  std::string deviceDisplayName(const UPowerDeviceInfo& info) {
    if (!info.model.empty()) {
      return info.model;
    }
    if (!info.vendor.empty()) {
      return info.vendor;
    }
    return i18n::tr("control-center.power.unknown-device");
  }

} // namespace

PowerTab::PowerTab(UPowerService* upower, PowerProfilesService* powerProfiles)
    : m_upower(upower), m_powerProfiles(powerProfiles) {}

std::unique_ptr<Flex> PowerTab::create() {
  const float scale = contentScale();

  auto tab = ui::column({
      .out = &m_root,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
  });

  auto scroll = ui::scrollView({
      .scrollbarVisible = true,
      .flexGrow = 1.0f,
      .configure = [](ScrollView& scrollView) {
        scrollView.clearFill();
        scrollView.clearBorder();
      },
  });

  auto* content = scroll->content();
  content->setDirection(FlexDirection::Vertical);
  content->setAlign(FlexAlign::Stretch);
  content->setGap(Style::spaceMd * scale);

  buildStatusCard(*content, scale);
  buildProfilesCard(*content, scale);
  buildHealthCard(*content, scale);
  buildPeripheralsCard(*content, scale);

  m_root->addChild(std::move(scroll));

  return tab;
}

void PowerTab::buildStatusCard(Flex& root, float scale) {
  if (m_upower == nullptr) {
    return;
  }

  auto card = ui::column({
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& section) {
        applySectionCardStyle(section, scale, opacity, borders);
      },
  });
  m_statusCard = card.get();

  card->addChild(makeCardHeaderRow(i18n::tr("control-center.power.battery"), scale));

  auto topRow = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::glyph({
          .out = &m_statusGlyph,
          .glyph = batteryGlyphName(0.0, BatteryState::Unknown),
          .glyphSize = Style::fontSizeTitle * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      }),
      ui::label({
          .out = &m_percentLabel,
          .text = "--",
          .fontSize = Style::fontSizeTitle * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .fontWeight = FontWeight::Bold,
      }),
      ui::label({
          .out = &m_stateLabel,
          .text = "",
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .flexGrow = 1.0f,
      })
  );
  card->addChild(std::move(topRow));

  card->addChild(
      ui::progressBar({
          .out = &m_levelBar,
          .fill = colorSpecFromRole(ColorRole::Primary),
          .track = colorSpecFromRole(ColorRole::Surface),
          .radius = Style::sliderTrackHeight * scale * 0.5f,
          .progress = 0.0f,
          .height = Style::sliderTrackHeight * scale,
      })
  );

  auto timeRow = ui::row(
      {.out = &m_timeRow, .align = FlexAlign::Center, .gap = Style::spaceXs * scale, .visible = false},
      ui::glyph({
          .glyph = "clock",
          .glyphSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      }),
      ui::label({
          .out = &m_timeLabel,
          .text = "",
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  card->addChild(std::move(timeRow));

  auto rateRow = ui::row(
      {.out = &m_rateRow, .align = FlexAlign::Center, .gap = Style::spaceXs * scale, .visible = false},
      ui::glyph({
          .glyph = "bolt",
          .glyphSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      }),
      ui::label({
          .out = &m_rateLabel,
          .text = "",
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  card->addChild(std::move(rateRow));

  root.addChild(std::move(card));
}

void PowerTab::buildProfilesCard(Flex& root, float scale) {
  if (m_powerProfiles == nullptr) {
    return;
  }

  m_profileOrder.clear();
  const auto& available = m_powerProfiles->profiles();
  for (const auto& candidate : powerProfileOrder()) {
    if (std::ranges::find(available, candidate) != available.end()) {
      m_profileOrder.emplace_back(candidate);
    }
  }
  if (m_profileOrder.empty()) {
    return;
  }

  auto card = ui::column({
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& section) {
        applySectionCardStyle(section, scale, opacity, borders);
      },
  });
  m_profilesCard = card.get();

  card->addChild(makeCardHeaderRow(i18n::tr("control-center.power.power-profile"), scale));

  std::vector<ui::SegmentedOption> options;
  options.reserve(m_profileOrder.size());
  for (const auto& profile : m_profileOrder) {
    options.push_back({.label = profileLabel(profile), .glyph = std::string(profileGlyphName(profile))});
  }

  card->addChild(
      ui::segmented({
          .out = &m_profiles,
          .options = std::move(options),
          .fontSize = Style::fontSizeCaption * scale,
          .scale = scale,
          .surfaceOpacity = panelCardOpacity(),
          .surfaceRole = ColorRole::Surface,
          .equalSegmentWidths = true,
          .onChange = [this](std::size_t index) {
            if (m_syncingProfiles || m_powerProfiles == nullptr || index >= m_profileOrder.size()) {
              return;
            }
            (void)m_powerProfiles->setActiveProfile(m_profileOrder[index]);
          },
      })
  );

  auto inhibitedRow = ui::row(
      {.out = &m_inhibitedRow, .align = FlexAlign::Center, .gap = Style::spaceXs * scale, .visible = false},
      ui::glyph({
          .glyph = "alert-triangle",
          .glyphSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::Error),
      }),
      ui::label({
          .out = &m_inhibitedLabel,
          .text = i18n::tr("control-center.power.performance-inhibited"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .flexGrow = 1.0f,
      })
  );
  card->addChild(std::move(inhibitedRow));

  root.addChild(std::move(card));
}

void PowerTab::buildHealthCard(Flex& root, float scale) {
  if (m_upower == nullptr) {
    return;
  }

  auto card = ui::column({
      .visible = false,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& section) {
        applySectionCardStyle(section, scale, opacity, borders);
      },
  });
  m_healthCard = card.get();

  auto header = makeCardHeaderRow(i18n::tr("control-center.power.health"), scale);
  header->addChild(
      ui::label({
          .out = &m_healthLabel,
          .text = "--",
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .fontWeight = FontWeight::Bold,
      })
  );
  card->addChild(std::move(header));

  card->addChild(
      ui::label({
          .text = i18n::tr("control-center.power.design-capacity"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );

  card->addChild(
      ui::progressBar({
          .out = &m_healthBar,
          .fill = colorSpecFromRole(ColorRole::Primary),
          .track = colorSpecFromRole(ColorRole::Surface),
          .radius = Style::sliderTrackHeight * scale * 0.5f,
          .progress = 0.0f,
          .height = Style::sliderTrackHeight * scale,
      })
  );

  root.addChild(std::move(card));
}

void PowerTab::buildPeripheralsCard(Flex& root, float scale) {
  if (m_upower == nullptr) {
    return;
  }

  auto card = ui::column({
      .visible = false,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& section) {
        applySectionCardStyle(section, scale, opacity, borders);
      },
  });
  m_peripheralsCard = card.get();

  card->addChild(makeCardHeaderRow(i18n::tr("control-center.power.peripherals"), scale));

  auto list = ui::column({.out = &m_peripheralsList, .align = FlexAlign::Stretch, .gap = Style::spaceSm * scale});
  card->addChild(std::move(list));

  root.addChild(std::move(card));
}

void PowerTab::onClose() {
  m_root = nullptr;
  m_statusCard = nullptr;
  m_statusGlyph = nullptr;
  m_percentLabel = nullptr;
  m_stateLabel = nullptr;
  m_levelBar = nullptr;
  m_timeRow = nullptr;
  m_timeLabel = nullptr;
  m_rateRow = nullptr;
  m_rateLabel = nullptr;
  m_profilesCard = nullptr;
  m_profiles = nullptr;
  m_inhibitedRow = nullptr;
  m_inhibitedLabel = nullptr;
  m_healthCard = nullptr;
  m_healthLabel = nullptr;
  m_healthBar = nullptr;
  m_peripheralsCard = nullptr;
  m_peripheralsList = nullptr;
  m_peripheralRows.clear();
  m_lastPeripheralKey.clear();
}

void PowerTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_root == nullptr) {
    return;
  }
  rebuildPeripherals();
  m_root->setSize(contentWidth, bodyHeight);
  m_root->layout(renderer);
}

void PowerTab::doUpdate(Renderer& /*renderer*/) {
  syncBatteryStatus();
  syncPowerProfiles();
  syncBatteryHealth();
  rebuildPeripherals();
}

void PowerTab::syncBatteryStatus() {
  if (m_statusCard == nullptr || m_upower == nullptr) {
    return;
  }

  const UPowerState& state = m_upower->state();
  m_statusCard->setVisible(state.isPresent);
  if (!state.isPresent) {
    return;
  }

  if (m_statusGlyph != nullptr) {
    m_statusGlyph->setGlyph(batteryGlyphName(state.percentage, state.state));
  }
  if (m_percentLabel != nullptr) {
    m_percentLabel->setText(std::format("{:.0f}%", state.percentage));
  }
  if (m_stateLabel != nullptr) {
    m_stateLabel->setText(batteryStateLabel(state.state));
  }
  if (m_levelBar != nullptr) {
    m_levelBar->setProgress(static_cast<float>(std::clamp(state.percentage / 100.0, 0.0, 1.0)));
    const bool low = state.state == BatteryState::Discharging && state.percentage <= 20.0;
    m_levelBar->setFill(colorSpecFromRole(low ? ColorRole::Error : ColorRole::Primary));
  }

  const bool charging = state.state == BatteryState::Charging || state.state == BatteryState::PendingCharge;
  const std::int64_t seconds = charging ? state.timeToFull : state.timeToEmpty;
  if (m_timeRow != nullptr) {
    const bool show = seconds > 0;
    m_timeRow->setVisible(show);
    if (show && m_timeLabel != nullptr) {
      const std::string duration = formatDuration(std::chrono::seconds{seconds});
      m_timeLabel->setText(
          i18n::tr(
              charging ? "control-center.power.time-to-full" : "control-center.power.time-to-empty", "time", duration
          )
      );
    }
  }

  if (m_rateRow != nullptr) {
    const bool show = state.energyRate > 0.0;
    m_rateRow->setVisible(show);
    if (show && m_rateLabel != nullptr) {
      m_rateLabel->setText(std::format("{:.1f} W", state.energyRate));
    }
  }
}

void PowerTab::syncPowerProfiles() {
  if (m_profiles == nullptr || m_powerProfiles == nullptr || m_profileOrder.empty()) {
    return;
  }

  const auto& active = m_powerProfiles->activeProfile();
  const auto it = std::ranges::find(m_profileOrder, active);
  if (it != m_profileOrder.end()) {
    const auto index = static_cast<std::size_t>(std::distance(m_profileOrder.begin(), it));
    if (index != m_profiles->selectedIndex()) {
      m_syncingProfiles = true;
      m_profiles->setSelectedIndex(index);
      m_syncingProfiles = false;
    }
  }

  if (m_inhibitedRow != nullptr) {
    m_inhibitedRow->setVisible(!m_powerProfiles->state().performanceInhibited.empty());
  }
}

void PowerTab::syncBatteryHealth() {
  if (m_healthCard == nullptr || m_upower == nullptr) {
    return;
  }

  const UPowerDeviceInfo* battery = m_upower->defaultSystemBattery();
  const bool hasHealth = battery != nullptr && battery->energyFullDesign > 0.0 && battery->energyFull > 0.0;
  m_healthCard->setVisible(hasHealth);
  if (!hasHealth) {
    return;
  }

  const double health = std::clamp(battery->energyFull / battery->energyFullDesign * 100.0, 0.0, 100.0);
  if (m_healthLabel != nullptr) {
    m_healthLabel->setText(std::format("{:.0f}%", health));
  }
  if (m_healthBar != nullptr) {
    m_healthBar->setProgress(static_cast<float>(health / 100.0));
    m_healthBar->setFill(colorSpecFromRole(healthRole(health)));
  }
}

void PowerTab::rebuildPeripherals() {
  if (m_peripheralsCard == nullptr || m_peripheralsList == nullptr || m_upower == nullptr) {
    return;
  }

  std::vector<UPowerDeviceInfo> peripherals;
  for (auto& device : m_upower->batteryDevices()) {
    if (!device.isLaptopBattery() && device.isPresent) {
      peripherals.push_back(std::move(device));
    }
  }

  std::string key;
  for (const auto& device : peripherals) {
    key += device.path;
    key += ';';
  }

  const bool structuralChange = key != m_lastPeripheralKey;
  if (structuralChange) {
    m_lastPeripheralKey = key;

    for (auto& entry : m_peripheralRows) {
      if (entry.row != nullptr) {
        m_peripheralsList->removeChild(entry.row);
      }
    }
    m_peripheralRows.clear();

    const float scale = contentScale();
    for (const auto& device : peripherals) {
      PeripheralRow entry;
      entry.path = device.path;
      auto row = ui::row(
          {.out = &entry.row, .align = FlexAlign::Center, .gap = Style::spaceSm * scale},
          ui::glyph({
              .glyph = batteryDeviceGlyphName(device.type),
              .glyphSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          }),
          ui::label({
              .out = &entry.nameLabel,
              .text = deviceDisplayName(device),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 1,
              .ellipsize = TextEllipsize::End,
              .flexGrow = 1.0f,
          }),
          ui::label({
              .out = &entry.pctLabel,
              .text = "",
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .minWidth = Style::controlHeightSm * scale,
          })
      );
      m_peripheralsList->addChild(std::move(row));
      m_peripheralRows.push_back(std::move(entry));
    }
  }

  m_peripheralsCard->setVisible(!peripherals.empty());

  for (std::size_t i = 0; i < m_peripheralRows.size() && i < peripherals.size(); ++i) {
    if (m_peripheralRows[i].pctLabel != nullptr) {
      m_peripheralRows[i].pctLabel->setText(std::format("{:.0f}%", peripherals[i].state.percentage));
    }
  }
}
