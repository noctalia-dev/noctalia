#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class SystemBus;

namespace sdbus {
  class IProxy;
} // namespace sdbus

enum class UPowerDeviceType : std::uint32_t {
  Unknown = 0,
  LinePower = 1,
  Battery = 2,
  Ups = 3,
  Monitor = 4,
  Mouse = 5,
  Keyboard = 6,
  Pda = 7,
  Phone = 8,
};

enum class BatteryState : std::uint8_t {
  Unknown = 0,
  Charging = 1,
  Discharging = 2,
  Empty = 3,
  FullyCharged = 4,
  PendingCharge = 5,
  PendingDischarge = 6,
};

[[nodiscard]] std::string batteryStateLabel(BatteryState state);

// Level-aware battery icon (battery-0..4 / charging / plugged), shared by the bar widget and Power tab.
[[nodiscard]] const char* batteryGlyphName(double percentage, BatteryState state);

// Icon for a non-laptop battery device (peripherals) keyed on its UPower device type.
[[nodiscard]] const char* batteryDeviceGlyphName(UPowerDeviceType type);

struct UPowerState {
  double percentage = 0.0;
  double energyRate = 0.0; // watts
  BatteryState state = BatteryState::Unknown;
  std::int64_t timeToEmpty = 0; // seconds
  std::int64_t timeToFull = 0;  // seconds
  double energy = 0.0;          // Wh
  bool isPresent = false;
  bool onBattery = false;

  bool operator==(const UPowerState&) const = default;
};

struct UPowerDeviceInfo {
  std::string path;
  std::string nativePath;
  std::string vendor;
  std::string model;
  std::string serial;
  double energyFull = 0.0;       // Wh
  double energyFullDesign = 0.0; // Wh
  UPowerDeviceType type = UPowerDeviceType::Unknown;
  bool powerSupply = false;
  bool isPresent = false;
  UPowerState state;

  bool operator==(const UPowerDeviceInfo&) const = default;

  [[nodiscard]] bool isLaptopBattery() const { return type == UPowerDeviceType::Battery && powerSupply; }
};

[[nodiscard]] bool upowerDeviceMatchesSelector(const UPowerDeviceInfo& info, std::string_view selector);

class UPowerService {
public:
  using ChangeCallback = std::function<void()>;

  explicit UPowerService(SystemBus& bus);

  void setChangeCallback(ChangeCallback callback);
  void refresh();

  [[nodiscard]] const UPowerState& state() const noexcept { return m_state; }
  [[nodiscard]] UPowerState stateForDevice(std::string_view selector) const;
  [[nodiscard]] std::vector<UPowerDeviceInfo> batteryDevices() const;
  [[nodiscard]] const UPowerDeviceInfo* defaultSystemBattery() const noexcept;
  [[nodiscard]] const UPowerDeviceInfo* deviceForSelector(std::string_view selector) const;

private:
  struct TrackedDevice {
    UPowerDeviceInfo info;
    std::unique_ptr<sdbus::IProxy> proxy;
  };

  [[nodiscard]] UPowerState readDefaultState() const;
  [[nodiscard]] UPowerState readDeviceState(sdbus::IProxy& proxy) const;
  [[nodiscard]] UPowerDeviceInfo readDeviceInfo(std::string path, sdbus::IProxy& proxy) const;
  void refreshDisplayDeviceProxy();
  void emitChangedIfNeeded(bool devicesChanged);
  void rescanDevices();
  void refreshDeviceStates();

  SystemBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_upowerProxy;
  std::unique_ptr<sdbus::IProxy> m_displayDeviceProxy;
  std::string m_displayDevicePath;
  std::vector<TrackedDevice> m_devices;
  std::optional<UPowerDeviceInfo> m_dummyDevice;
  UPowerState m_state;
  ChangeCallback m_changeCallback;
};
