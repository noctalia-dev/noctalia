#include "hooks/battery_hook_state.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

namespace {

  std::optional<HookKind> batteryStateHook(BatteryState state) {
    switch (state) {
    case BatteryState::Charging:
      return HookKind::BatteryCharging;
    case BatteryState::Discharging:
      return HookKind::BatteryDischarging;
    case BatteryState::FullyCharged:
    case BatteryState::PendingCharge:
      return HookKind::BatteryPlugged;
    case BatteryState::Unknown:
    case BatteryState::Empty:
    case BatteryState::PendingDischarge:
      return std::nullopt;
    }
    return std::nullopt;
  }

  std::string_view batteryStateHookValue(BatteryState state) {
    switch (state) {
    case BatteryState::Charging:
      return "charging";
    case BatteryState::Discharging:
      return "discharging";
    case BatteryState::Empty:
      return "empty";
    case BatteryState::FullyCharged:
      return "fully_charged";
    case BatteryState::PendingCharge:
      return "pending_charge";
    case BatteryState::PendingDischarge:
      return "pending_discharge";
    case BatteryState::Unknown:
      return "unknown";
    }
    return "unknown";
  }

  int normalizedBatteryPercent(double percentage) {
    if (!std::isfinite(percentage)) {
      return 0;
    }
    return std::clamp(static_cast<int>(std::lround(percentage)), 0, 100);
  }

} // namespace

void BatteryHookState::reset(const UPowerState& state) {
  m_initialized = true;
  m_lastStateHook = batteryStateHook(state.state);
  if (state.isPresent) {
    m_lastPercent = normalizedBatteryPercent(state.percentage);
  } else {
    m_lastPercent.reset();
  }
}

std::vector<BatteryHookState::Event> BatteryHookState::update(const UPowerState& state) {
  std::vector<Event> events;
  if (!m_initialized) {
    reset(state);
    return events;
  }

  if (!state.isPresent) {
    m_lastStateHook.reset();
    m_lastPercent.reset();
    return events;
  }

  if (const auto stateHook = batteryStateHook(state.state)) {
    if (!m_lastStateHook.has_value() || *m_lastStateHook != *stateHook) {
      events.push_back({*stateHook, {}});
    }
    m_lastStateHook = stateHook;
  }

  const int percent = normalizedBatteryPercent(state.percentage);
  if (m_lastPercent.has_value() && *m_lastPercent != percent) {
    events.push_back(
        {HookKind::BatteryPercentageChanged,
         {{"NOCTALIA_BATTERY_STATE", std::string(batteryStateHookValue(state.state))},
          {"NOCTALIA_BATTERY_PERCENT", std::to_string(percent)}}}
    );
  }
  m_lastPercent = percent;
  return events;
}
