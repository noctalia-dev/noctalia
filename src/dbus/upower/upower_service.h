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

// Mirrors the org.freedesktop.UPower.Device "Type" wire enum.
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
  MediaPlayer = 9,
  Tablet = 10,
  Computer = 11,
  GamingInput = 12,
  Pen = 13,
  Touchpad = 14,
  Modem = 15,
  Network = 16,
  Headset = 17,
  Speakers = 18,
  Headphones = 19,
  Video = 20,
  OtherAudio = 21,
  RemoteControl = 22,
  Printer = 23,
  Scanner = 24,
  Camera = 25,
  Wearable = 26,
  Toy = 27,
  BluetoothGeneric = 28,
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

enum class ChargeLimitOperationError : std::uint8_t {
  None,
  PermissionDenied,
  Failed,
};

struct UPowerChargeLimitState {
  // Values configured in UPower. They are presets, not necessarily what the kernel currently applies.
  std::optional<std::uint32_t> configuredStart;
  std::optional<std::uint32_t> configuredEnd;
  // Read-only values currently exposed by the kernel.
  std::optional<std::uint32_t> effectiveStart;
  std::optional<std::uint32_t> effectiveEnd;
  std::optional<std::uint32_t> supportedSettings;
  bool supported = false;
  bool methodAvailable = false;
  bool enabledAvailable = false;
  bool enabled = false;
  bool requestPending = false;
  std::optional<bool> requestedEnabled;
  ChargeLimitOperationError operationError = ChargeLimitOperationError::None;

  bool operator==(const UPowerChargeLimitState&) const = default;
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
  UPowerChargeLimitState chargeLimit;

  bool operator==(const UPowerDeviceInfo&) const = default;

  [[nodiscard]] bool isLaptopBattery() const { return type == UPowerDeviceType::Battery && powerSupply; }
  [[nodiscard]] bool sameCatalogEntry(const UPowerDeviceInfo& other) const;
};

enum class UPowerChangeOrigin : std::uint8_t {
  DeviceState,
  ChargeLimit,
};

struct UPowerChange {
  UPowerChangeOrigin origin = UPowerChangeOrigin::DeviceState;
  bool deviceCatalogChanged = false;
};

[[nodiscard]] bool upowerDeviceMatchesSelector(const UPowerDeviceInfo& info, std::string_view selector);

class UPowerService {
public:
  using ChangeOrigin = UPowerChangeOrigin;
  using ChangeCallback = std::function<void(const UPowerChange&)>;

  explicit UPowerService(SystemBus& bus);
  ~UPowerService();

  void setChangeCallback(ChangeCallback callback);
  void refresh();

  [[nodiscard]] const UPowerState& state() const noexcept { return m_state; }
  [[nodiscard]] UPowerState stateForDevice(std::string_view selector) const;
  [[nodiscard]] std::vector<UPowerDeviceInfo> batteryDevices() const;
  [[nodiscard]] const UPowerDeviceInfo* defaultSystemBattery() const noexcept;
  [[nodiscard]] const UPowerDeviceInfo* deviceForSelector(std::string_view selector) const;
  // Peripheral (non-system) battery whose serial matches, compared case-insensitively because
  // MAC-style serials differ in case between BlueZ-backed and kernel-backed UPower devices.
  [[nodiscard]] const UPowerDeviceInfo* peripheralBatteryForSerial(std::string_view serial) const;
  // Applies or removes UPower's configured preset. Completion is reflected in the device's
  // chargeLimit fields and delivered through the normal change callback.
  [[nodiscard]] bool enableChargeThreshold(std::string_view devicePath, bool enabled);

private:
  struct TrackedDevice {
    UPowerDeviceInfo info;
    std::shared_ptr<sdbus::IProxy> proxy;
    std::optional<bool> chargeThresholdMethodAvailable;
    std::optional<std::uint64_t> chargeThresholdRequestId;
  };

  struct RefreshChanges {
    bool devicesChanged = false;
    bool chargeLimitChanged = false;
    bool deviceCatalogChanged = false;
  };

  [[nodiscard]] UPowerState readDefaultState() const;
  [[nodiscard]] UPowerState readDeviceState(sdbus::IProxy& proxy) const;
  [[nodiscard]] UPowerDeviceInfo readDeviceInfoBase(std::string path, sdbus::IProxy& proxy) const;
  [[nodiscard]] UPowerChargeLimitState readChargeLimitState(
      const UPowerDeviceInfo& info, sdbus::IProxy& proxy, std::optional<bool>& chargeThresholdMethodAvailable
  ) const;
  [[nodiscard]] UPowerDeviceInfo
  readDeviceInfo(std::string path, sdbus::IProxy& proxy, std::optional<bool>& chargeThresholdMethodAvailable) const;
  void refreshDisplayDeviceProxy();
  [[nodiscard]] bool refreshDefaultState();
  void emitChangedIfNeeded(bool devicesChanged, bool chargeLimitChanged = false, bool deviceCatalogChanged = false);
  void emitControlOperationChanged();
  void invalidateChargeThresholdRequest(std::string_view devicePath);
  void rescanDevices();
  [[nodiscard]] RefreshChanges refreshDevice(std::string_view devicePath, bool refreshChargeLimit);
  [[nodiscard]] RefreshChanges refreshTrackedDevice(TrackedDevice& device, bool refreshChargeLimit);
  [[nodiscard]] RefreshChanges refreshDeviceStates();

  SystemBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_upowerProxy;
  std::unique_ptr<sdbus::IProxy> m_displayDeviceProxy;
  std::string m_displayDevicePath;
  std::vector<TrackedDevice> m_devices;
  std::optional<UPowerDeviceInfo> m_dummyDevice;
  UPowerState m_state;
  ChangeCallback m_changeCallback;
  std::shared_ptr<int> m_lifetimeToken = std::make_shared<int>(0);
  std::uint64_t m_nextChargeThresholdRequestId = 1;
};
