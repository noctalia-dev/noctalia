#include "hooks/battery_hook_state.h"
#include "tests/test_check.h"

#include <string_view>
#include <vector>

namespace {

  UPowerState batteryState(BatteryState state, double percent, bool present = true) {
    UPowerState out;
    out.state = state;
    out.percentage = percent;
    out.isPresent = present;
    return out;
  }

  void expectSingleHook(const std::vector<BatteryHookState::Event>& events, HookKind kind) {
    TEST_CHECK(events.size() == 1);
    TEST_CHECK(events[0].kind == kind);
  }

  std::string_view envValue(const BatteryHookState::Event& event, std::string_view key) {
    for (const auto& [envKey, envValue] : event.env) {
      if (std::string_view(envKey) == key) {
        return envValue;
      }
    }
    return {};
  }

} // namespace

int main() {
  BatteryHookState hooks;
  hooks.reset(batteryState(BatteryState::Charging, 50.0));

  TEST_CHECK(hooks.update(batteryState(BatteryState::Unknown, 50.0)).empty());
  TEST_CHECK(hooks.update(batteryState(BatteryState::Charging, 50.0)).empty());

  auto events = hooks.update(batteryState(BatteryState::Discharging, 50.0));
  expectSingleHook(events, HookKind::BatteryDischarging);
  TEST_CHECK(events[0].env.empty());

  TEST_CHECK(hooks.update(batteryState(BatteryState::PendingDischarge, 50.0)).empty());
  TEST_CHECK(hooks.update(batteryState(BatteryState::Discharging, 50.0)).empty());

  events = hooks.update(batteryState(BatteryState::Charging, 50.0));
  expectSingleHook(events, HookKind::BatteryCharging);
  TEST_CHECK(events[0].env.empty());

  events = hooks.update(batteryState(BatteryState::Charging, 51.0));
  expectSingleHook(events, HookKind::BatteryPercentageChanged);
  TEST_CHECK(envValue(events[0], "NOCTALIA_BATTERY_STATE") == "charging");
  TEST_CHECK(envValue(events[0], "NOCTALIA_BATTERY_PERCENT") == "51");

  events = hooks.update(batteryState(BatteryState::FullyCharged, 100.0));
  TEST_CHECK(events.size() == 2);
  TEST_CHECK(events[0].kind == HookKind::BatteryPlugged);
  TEST_CHECK(events[0].env.empty());
  TEST_CHECK(events[1].kind == HookKind::BatteryPercentageChanged);
  TEST_CHECK(envValue(events[1], "NOCTALIA_BATTERY_STATE") == "fully_charged");
  TEST_CHECK(envValue(events[1], "NOCTALIA_BATTERY_PERCENT") == "100");
  TEST_CHECK(hooks.update(batteryState(BatteryState::PendingCharge, 100.0)).empty());

  events = hooks.update(batteryState(BatteryState::Charging, 100.0));
  expectSingleHook(events, HookKind::BatteryCharging);
  TEST_CHECK(events[0].env.empty());

  TEST_CHECK(hooks.update(batteryState(BatteryState::Unknown, 0.0, false)).empty());
  events = hooks.update(batteryState(BatteryState::Discharging, 40.0));
  expectSingleHook(events, HookKind::BatteryDischarging);
  TEST_CHECK(events[0].env.empty());

  return 0;
}
