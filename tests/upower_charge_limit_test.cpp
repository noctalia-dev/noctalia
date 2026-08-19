#include "dbus/upower/upower_charge_limit_support.h"
#include "dbus/upower/upower_service.h"
#include "shell/control_center/tabs/power_tab.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>
#include <unistd.h>

class PowerTabTestAccess {
public:
  enum class Mode {
    Unsupported,
    UPowerActive,
    UPowerDisabled,
    ExternallyManaged,
    FirmwareManaged,
    ReadOnly,
  };

  static Mode mode(const UPowerChargeLimitState& state) {
    switch (PowerTab::classifyChargeLimit(state)) {
    case PowerTab::ChargeLimitMode::UPowerActive:
      return Mode::UPowerActive;
    case PowerTab::ChargeLimitMode::UPowerDisabled:
      return Mode::UPowerDisabled;
    case PowerTab::ChargeLimitMode::ExternallyManaged:
      return Mode::ExternallyManaged;
    case PowerTab::ChargeLimitMode::FirmwareManaged:
      return Mode::FirmwareManaged;
    case PowerTab::ChargeLimitMode::ReadOnly:
      return Mode::ReadOnly;
    case PowerTab::ChargeLimitMode::Unsupported:
    default:
      return Mode::Unsupported;
    }
  }

  static std::tuple<bool, bool, bool> control(const UPowerChargeLimitState& state) {
    const auto result = PowerTab::chargeLimitControlState(state);
    return {result.visible, result.checked, result.enabled};
  }

  static bool visible(const UPowerChargeLimitState& state) { return PowerTab::shouldShowChargeLimit(state); }
};

namespace {

  class TempTree {
  public:
    TempTree() {
      const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
      root = std::filesystem::temp_directory_path() / ("noctalia-charge-limit-" + std::to_string(stamp));
      std::filesystem::create_directories(root / "BAT0");
    }

    ~TempTree() {
      std::error_code error;
      std::filesystem::permissions(
          root / "BAT0" / "charge_control_end_threshold", std::filesystem::perms::owner_all,
          std::filesystem::perm_options::add, error
      );
      std::filesystem::remove_all(root, error);
    }

    void write(std::string_view name, std::string_view value) const {
      std::ofstream output(root / "BAT0" / name);
      output << value;
    }

    std::filesystem::path root;
  };

  UPowerChargeLimitState supportedState(bool enabled) {
    UPowerChargeLimitState state;
    state.supported = true;
    state.methodAvailable = true;
    state.enabledAvailable = true;
    state.enabled = enabled;
    return state;
  }

} // namespace

int main() {
  TempTree tree;

  tree.write("charge_control_start_threshold", "75\n");
  tree.write("charge_control_end_threshold", " 80 \n");
  auto probe = upower::detail::readChargeThresholdsFromSysfs("BAT0", tree.root);
  assert(probe.start == 75U);
  assert(probe.end == 80U);

  probe = upower::detail::readChargeThresholdsFromSysfs("/sys/devices/platform/test/power_supply/BAT0", tree.root);
  assert(probe.start == 75U);
  assert(probe.end == 80U);

  std::filesystem::remove(tree.root / "BAT0" / "charge_control_end_threshold");
  probe = upower::detail::readChargeThresholdsFromSysfs("BAT0", tree.root);
  assert(probe.start == 75U);
  assert(!probe.end.has_value());

  std::filesystem::remove(tree.root / "BAT0" / "charge_control_start_threshold");
  tree.write("charge_control_end_threshold", "85");
  probe = upower::detail::readChargeThresholdsFromSysfs("BAT0", tree.root);
  assert(!probe.start.has_value());
  assert(probe.end == 85U);

  probe = upower::detail::readChargeThresholdsFromSysfs("BAT1", tree.root);
  assert(!probe.start.has_value() && !probe.end.has_value());

  for (const std::string value : {"nope", "80 percent", "-1", "101", "4294967296"}) {
    tree.write("charge_control_end_threshold", value);
    probe = upower::detail::readChargeThresholdsFromSysfs("BAT0", tree.root);
    assert(!probe.end.has_value());
  }

  tree.write("charge_control_end_threshold", "80");
  std::filesystem::permissions(
      tree.root / "BAT0" / "charge_control_end_threshold", std::filesystem::perms::none,
      std::filesystem::perm_options::replace
  );
  probe = upower::detail::readChargeThresholdsFromSysfs("BAT0", tree.root);
  if (geteuid() != 0) {
    assert(!probe.end.has_value());
  }

  for (const std::string unsafe :
       {"", ".", "..", "../BAT0", "BAT0/../../etc", "/sys/devices/../power_supply/BAT0", "/etc/BAT0",
        "/tmp/power_supply/BAT0", "/sys/class/power_supply/BAT0/extra", "/sys/class/power_supply/BAT 0", "BAT 0",
        "BAT0\\x"}) {
    probe = upower::detail::readChargeThresholdsFromSysfs(unsafe, tree.root);
    assert(!probe.start.has_value() && !probe.end.has_value());
  }

  UPowerChargeLimitState state;
  using Mode = PowerTabTestAccess::Mode;
  assert(PowerTabTestAccess::mode(state) == Mode::Unsupported);
  assert(!PowerTabTestAccess::visible(state));

  state.supported = true;
  assert(PowerTabTestAccess::mode(state) == Mode::ReadOnly);
  assert(!PowerTabTestAccess::visible(state));

  state = supportedState(true);
  state.supportedSettings = 3U;
  state.configuredStart = 75U;
  state.configuredEnd = 80U;
  state.effectiveStart = 75U;
  state.effectiveEnd = 80U;
  assert(PowerTabTestAccess::mode(state) == Mode::UPowerActive);
  assert(PowerTabTestAccess::visible(state));

  state = supportedState(false);
  state.effectiveStart = 0U;
  state.effectiveEnd = 100U;
  assert(PowerTabTestAccess::mode(state) == Mode::UPowerDisabled);

  state = supportedState(false);
  state.configuredStart = 75U;
  state.configuredEnd = 80U;
  state.effectiveStart = 75U;
  state.effectiveEnd = 80U;
  assert(PowerTabTestAccess::mode(state) == Mode::ExternallyManaged);
  assert(PowerTabTestAccess::control(state) == std::tuple(true, true, false));

  state.requestPending = true;
  state.requestedEnabled = false;
  assert(PowerTabTestAccess::mode(state) == Mode::UPowerDisabled);
  assert(PowerTabTestAccess::control(state) == std::tuple(true, false, false));

  state = supportedState(true);
  state.supportedSettings = 4U;
  assert(PowerTabTestAccess::mode(state) == Mode::FirmwareManaged);
  assert(PowerTabTestAccess::visible(state));

  state.methodAvailable = false;
  assert(PowerTabTestAccess::mode(state) == Mode::FirmwareManaged);
  assert(PowerTabTestAccess::control(state) == std::tuple(false, true, false));

  state = supportedState(true);
  state.supportedSettings = 2U;
  state.configuredEnd = 80U;
  state.effectiveEnd = 80U;
  assert(PowerTabTestAccess::mode(state) == Mode::UPowerActive);

  state.enabledAvailable = false;
  assert(PowerTabTestAccess::mode(state) == Mode::ReadOnly);

  state = supportedState(false);
  state.methodAvailable = false;
  assert(PowerTabTestAccess::mode(state) == Mode::ReadOnly);
  assert(PowerTabTestAccess::control(state) == std::tuple(false, false, false));

  state = {};
  state.effectiveEnd = 100U;
  assert(PowerTabTestAccess::mode(state) == Mode::ReadOnly);

  state = {};
  state.effectiveStart = 70U;
  assert(PowerTabTestAccess::mode(state) == Mode::ReadOnly);
  assert(PowerTabTestAccess::visible(state));

  state = supportedState(true);
  state.configuredStart = 70U;
  state.configuredEnd = 80U;
  state.effectiveStart = 75U;
  state.effectiveEnd = 85U;
  state.requestPending = true;
  state.requestedEnabled = false;
  assert(state.configuredStart != state.effectiveStart);
  assert(state.configuredEnd != state.effectiveEnd);
  assert(PowerTabTestAccess::mode(state) == Mode::UPowerActive);
  assert(PowerTabTestAccess::control(state) == std::tuple(true, false, false));

  // A reconciled disabled state presents an enabled control after a failed operation.
  state = supportedState(false);
  state.operationError = ChargeLimitOperationError::PermissionDenied;
  assert(PowerTabTestAccess::control(state) == std::tuple(true, false, true));

  // A reconciled enabled state presents an enabled, checked control.
  state = supportedState(true);
  assert(PowerTabTestAccess::control(state) == std::tuple(true, true, true));

  state = supportedState(false);
  state.effectiveStart = 75U;
  state.effectiveEnd = 80U;
  assert(PowerTabTestAccess::control(state) == std::tuple(true, true, false));

  // Keep the disabled control visible when UPower reports the externally applied
  // thresholds but does not advertise support for changing them.
  state.supported = false;
  state.methodAvailable = false;
  assert(PowerTabTestAccess::mode(state) == Mode::ExternallyManaged);
  assert(PowerTabTestAccess::control(state) == std::tuple(true, true, false));

  return 0;
}
