#include "shell/control_center/tabs/power_tab.h"

#include "dbus/power/power_profiles_service.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/toggle.h"
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

  std::string thresholdBehavior(const std::optional<std::uint32_t>& start, const std::optional<std::uint32_t>& end) {
    if (start.has_value() && end.has_value()) {
      return i18n::tr(
          "control-center.power.charging.starts-stops", "start", std::to_string(*start), "end", std::to_string(*end)
      );
    }
    if (start.has_value()) {
      return i18n::tr("control-center.power.charging.starts", "start", std::to_string(*start));
    }
    if (end.has_value()) {
      return i18n::tr("control-center.power.charging.stops", "end", std::to_string(*end));
    }
    return {};
  }

  bool sameThresholds(const UPowerChargeLimitState& state) {
    return state.configuredStart == state.effectiveStart && state.configuredEnd == state.effectiveEnd;
  }

  bool hasRestrictiveThreshold(const UPowerChargeLimitState& state) {
    return (state.effectiveStart.has_value() && *state.effectiveStart > 0U)
        || (state.effectiveEnd.has_value() && *state.effectiveEnd < 100U);
  }

} // namespace

PowerTab::ChargeLimitMode PowerTab::classifyChargeLimit(const UPowerChargeLimitState& state) noexcept {
  if (!state.requestPending && state.enabledAvailable && !state.enabled && hasRestrictiveThreshold(state)) {
    return ChargeLimitMode::ExternallyManaged;
  }

  const bool firmware = state.supportedSettings.has_value() && ((*state.supportedSettings & 4U) != 0U);
  const bool hasNumericThreshold = state.configuredStart.has_value()
      || state.configuredEnd.has_value()
      || state.effectiveStart.has_value()
      || state.effectiveEnd.has_value();
  const bool controllable = state.supported && state.methodAvailable && state.enabledAvailable;
  if (controllable) {
    if (!state.enabled) {
      return ChargeLimitMode::UPowerDisabled;
    }
    return firmware && !hasNumericThreshold ? ChargeLimitMode::FirmwareManaged : ChargeLimitMode::UPowerActive;
  }

  if (state.supported && firmware && !hasNumericThreshold) {
    return ChargeLimitMode::FirmwareManaged;
  }
  if (state.supported || hasNumericThreshold) {
    return ChargeLimitMode::ReadOnly;
  }
  return ChargeLimitMode::Unsupported;
}

PowerTab::ChargeLimitControlState PowerTab::chargeLimitControlState(const UPowerChargeLimitState& state) noexcept {
  const ChargeLimitMode mode = classifyChargeLimit(state);
  ChargeLimitControlState control;
  const bool controllable = state.supported && state.methodAvailable && state.enabledAvailable;
  control.visible = mode == ChargeLimitMode::ExternallyManaged
      || (controllable
          && (mode == ChargeLimitMode::UPowerActive
              || mode == ChargeLimitMode::UPowerDisabled
              || mode == ChargeLimitMode::FirmwareManaged));
  control.checked = mode == ChargeLimitMode::ExternallyManaged ? true : state.requestedEnabled.value_or(state.enabled);
  control.enabled = control.visible && mode != ChargeLimitMode::ExternallyManaged && !state.requestPending;
  return control;
}

bool PowerTab::shouldShowChargeLimit(const UPowerChargeLimitState& state) noexcept {
  const bool hasNumericThreshold = state.configuredStart.has_value()
      || state.configuredEnd.has_value()
      || state.effectiveStart.has_value()
      || state.effectiveEnd.has_value();
  const bool firmwareManaged = state.supportedSettings.has_value() && ((*state.supportedSettings & 4U) != 0U);
  return hasNumericThreshold || firmwareManaged || chargeLimitControlState(state).visible;
}

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
      .flexGrow = 1.0F,
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
  buildChargingCard(*content, scale);
  buildHealthCard(*content, scale);
  buildPeripheralsCard(*content, scale);

  m_root->addChild(std::move(scroll));

  return tab;
}

void PowerTab::buildChargingCard(Flex& root, float scale) {
  if (m_upower == nullptr) {
    return;
  }

  auto card = ui::column({
      .visible = false,
      .configure = [scale, opacity = panelCardOpacity()](Flex& section) {
        applySectionCardStyle(section, scale, opacity);
      },
  });
  m_chargingCard = card.get();
  card->addChild(makeCardHeaderRow(i18n::tr("control-center.power.charging.title"), scale));
  card->addChild(
      ui::column({
          .out = &m_chargingList,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceMd * scale,
      })
  );
  root.addChild(std::move(card));
}

void PowerTab::buildStatusCard(Flex& root, float scale) {
  if (m_upower == nullptr) {
    return;
  }

  auto card = ui::column({
      .configure = [scale, opacity = panelCardOpacity()](Flex& section) {
        applySectionCardStyle(section, scale, opacity);
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
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      }),
      ui::label({
          .out = &m_stateLabel,
          .text = "",
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .flexGrow = 1.0F,
      })
  );
  card->addChild(std::move(topRow));

  card->addChild(
      ui::progressBar({
          .out = &m_levelBar,
          .fill = colorSpecFromRole(ColorRole::Primary),
          .track = colorSpecFromRole(ColorRole::Surface),
          .radius = Style::sliderTrackHeight * scale * 0.5F,
          .progress = 0.0F,
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
      .configure = [scale, opacity = panelCardOpacity()](Flex& section) {
        applySectionCardStyle(section, scale, opacity);
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
          .flexGrow = 1.0F,
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
      .configure = [scale, opacity = panelCardOpacity()](Flex& section) {
        applySectionCardStyle(section, scale, opacity);
      },
  });
  m_healthCard = card.get();

  auto header = makeCardHeaderRow(i18n::tr("control-center.power.health"), scale);
  header->addChild(
      ui::label({
          .out = &m_healthLabel,
          .text = "--",
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
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
          .radius = Style::sliderTrackHeight * scale * 0.5F,
          .progress = 0.0F,
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
      .configure = [scale, opacity = panelCardOpacity()](Flex& section) {
        applySectionCardStyle(section, scale, opacity);
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
  m_chargingCard = nullptr;
  m_chargingList = nullptr;
  m_chargeLimitRows.clear();
  m_lastChargeLimitKey.clear();
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
  rebuildChargeLimits();
  m_root->setSize(contentWidth, bodyHeight);
  m_root->layout(renderer);
}

void PowerTab::doUpdate(Renderer& /*renderer*/) {
  syncBatteryStatus();
  rebuildChargeLimits();
  syncPowerProfiles();
  syncBatteryHealth();
  rebuildPeripherals();
}

void PowerTab::rebuildChargeLimits() {
  if (m_chargingCard == nullptr || m_chargingList == nullptr || m_upower == nullptr) {
    return;
  }

  std::vector<UPowerDeviceInfo> batteries;
  for (auto& device : m_upower->batteryDevices()) {
    if (device.isLaptopBattery() && device.isPresent && shouldShowChargeLimit(device.chargeLimit)) {
      batteries.push_back(std::move(device));
    }
  }

  std::string key = std::to_string(batteries.size()) + ':';
  for (const auto& battery : batteries) {
    key += battery.path;
    key += ';';
  }

  if (key != m_lastChargeLimitKey) {
    m_lastChargeLimitKey = key;
    for (auto& entry : m_chargeLimitRows) {
      if (entry.row != nullptr) {
        m_chargingList->removeChild(entry.row);
      }
    }
    m_chargeLimitRows.clear();

    const float scale = contentScale();
    const bool showNames = batteries.size() > 1;
    for (const auto& battery : batteries) {
      ChargeLimitRow entry;
      auto row = ui::column({
          .out = &entry.row,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * scale,
      });
      row->addChild(
          ui::label({
              .out = &entry.nameLabel,
              .text = deviceDisplayName(battery),
              .fontSize = Style::fontSizeBody * scale,
              .fontWeight = FontWeight::Bold,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 1,
              .ellipsize = TextEllipsize::End,
              .visible = showNames,
          })
      );
      row->addChild(
          ui::label({
              .out = &entry.behaviorLabel,
              .text = "",
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurface),
          })
      );
      row->addChild(
          ui::label({
              .out = &entry.configuredLabel,
              .text = "",
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .visible = false,
          })
      );
      row->addChild(
          ui::row(
              {.out = &entry.controlRow, .align = FlexAlign::Center, .gap = Style::spaceSm * scale, .visible = false},
              ui::label({
                  .out = &entry.controlLabel,
                  .text = i18n::tr("control-center.power.charging.use-thresholds"),
                  .fontSize = Style::fontSizeCaption * scale,
                  .color = colorSpecFromRole(ColorRole::OnSurface),
                  .flexGrow = 1.0F,
              }),
              ui::toggle({
                  .out = &entry.toggle,
                  .checkedImmediate = battery.chargeLimit.enabled,
                  .toggleSize = ToggleSize::Small,
                  .scale = scale,
                  .onChange = [this, path = battery.path](bool checked) {
                    if (m_upower != nullptr) {
                      (void)m_upower->enableChargeThreshold(path, checked);
                    }
                  },
              })
          )
      );
      row->addChild(
          ui::label({
              .out = &entry.managementLabel,
              .text = "",
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .visible = false,
          })
      );
      row->addChild(
          ui::label({
              .out = &entry.errorLabel,
              .text = "",
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::Error),
              .visible = false,
          })
      );
      m_chargingList->addChild(std::move(row));
      m_chargeLimitRows.push_back(entry);
    }
  }

  m_chargingCard->setVisible(!batteries.empty());
  for (std::size_t i = 0; i < batteries.size() && i < m_chargeLimitRows.size(); ++i) {
    const auto& state = batteries[i].chargeLimit;
    auto& row = m_chargeLimitRows[i];
    const ChargeLimitMode mode = classifyChargeLimit(state);
    const bool permitsFullCharge = (state.enabledAvailable && !state.enabled && !hasRestrictiveThreshold(state))
        || (state.effectiveEnd == 100U && (!state.effectiveStart.has_value() || state.effectiveStart == 0U));
    if (row.nameLabel != nullptr) {
      row.nameLabel->setText(deviceDisplayName(batteries[i]));
    }
    const ChargeLimitControlState control = chargeLimitControlState(state);
    const std::string effective = thresholdBehavior(state.effectiveStart, state.effectiveEnd);
    const std::string configured = thresholdBehavior(state.configuredStart, state.configuredEnd);
    const bool presentConfiguredAsPrimary =
        control.visible && mode != ChargeLimitMode::ExternallyManaged && control.checked && !configured.empty();
    std::string behavior = control.visible && mode != ChargeLimitMode::ExternallyManaged && !control.checked
        ? i18n::tr("control-center.power.charging.full-charge")
        : (presentConfiguredAsPrimary
               ? configured
               : (permitsFullCharge ? i18n::tr("control-center.power.charging.full-charge") : effective));
    if (behavior.empty()) {
      if (mode == ChargeLimitMode::FirmwareManaged) {
        behavior = i18n::tr("control-center.power.charging.firmware-managed");
      } else {
        behavior = i18n::tr("control-center.power.charging.unreadable");
      }
    }
    row.behaviorLabel->setText(behavior);
    row.behaviorLabel->setColor(colorSpecFromRole(ColorRole::OnSurface));

    const bool showConfigured = (mode == ChargeLimitMode::ExternallyManaged || !control.visible)
        && !configured.empty()
        && !sameThresholds(state);
    row.configuredLabel->setVisible(showConfigured);
    if (showConfigured) {
      row.configuredLabel->setText(i18n::tr("control-center.power.charging.configured", "limits", configured));
    }

    std::string management;
    if (mode == ChargeLimitMode::ExternallyManaged) {
      management = i18n::tr("control-center.power.charging.externally-managed");
    } else if (mode == ChargeLimitMode::Unsupported) {
      management = i18n::tr("control-center.power.charging.unsupported");
    } else if (mode == ChargeLimitMode::ReadOnly || (mode == ChargeLimitMode::FirmwareManaged && !control.visible)) {
      management = i18n::tr("control-center.power.charging.display-only");
    }
    row.managementLabel->setVisible(!management.empty());
    if (!management.empty()) {
      row.managementLabel->setText(management);
      row.managementLabel->setColor(
          colorSpecFromRole(ColorRole::OnSurfaceVariant, mode == ChargeLimitMode::ExternallyManaged ? 0.55F : 1.0F)
      );
    }

    row.controlRow->setVisible(control.visible);
    if (control.visible) {
      row.controlLabel->setColor(
          mode == ChargeLimitMode::ExternallyManaged ? colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.55F)
                                                     : colorSpecFromRole(ColorRole::OnSurface)
      );
      row.toggle->setCheckedImmediate(control.checked);
      row.toggle->setEnabled(control.enabled);
    }

    const bool hasError = state.operationError != ChargeLimitOperationError::None;
    row.errorLabel->setVisible(hasError);
    if (hasError) {
      row.errorLabel->setText(
          i18n::tr(
              state.operationError == ChargeLimitOperationError::PermissionDenied
                  ? "control-center.power.charging.error-denied"
                  : "control-center.power.charging.error-failed"
          )
      );
    }
  }
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
    m_percentLabel->setText(std::format("{:.0F}%", state.percentage));
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
      m_rateLabel->setText(std::format("{:.1F} W", state.energyRate));
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
    m_healthLabel->setText(std::format("{:.0F}%", health));
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
              .flexGrow = 1.0F,
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
      m_peripheralRows[i].pctLabel->setText(std::format("{:.0F}%", peripherals[i].state.percentage));
    }
  }
}
