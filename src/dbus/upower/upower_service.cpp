#include "dbus/upower/upower_service.h"

#include "core/log.h"
#include "dbus/system_bus.h"
#include "i18n/i18n.h"
#include "util/string_utils.h"
#include "util/sys_utils.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <string_view>
#include <utility>
#include <vector>

namespace {

  const sdbus::ServiceName kUpowerBusName{"org.freedesktop.UPower"};
  const sdbus::ObjectPath kUpowerObjectPath{"/org/freedesktop/UPower"};
  constexpr auto kUpowerInterface = "org.freedesktop.UPower";
  constexpr auto kDeviceInterface = "org.freedesktop.UPower.Device";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";

} // namespace

std::string batteryStateLabel(BatteryState state) {
  switch (state) {
  case BatteryState::Charging:
    return i18n::tr("power.battery.states.charging");
  case BatteryState::Discharging:
    return i18n::tr("power.battery.states.discharging");
  case BatteryState::FullyCharged:
    return i18n::tr("power.battery.states.plugged-in");
  case BatteryState::Empty:
    return i18n::tr("power.battery.states.empty");
  case BatteryState::PendingCharge:
    return i18n::tr("power.battery.states.plugged-in");
  case BatteryState::PendingDischarge:
    return i18n::tr("power.battery.states.pending-discharge");
  case BatteryState::Unknown:
  default:
    return i18n::tr("power.battery.states.battery");
  }
}

const char* batteryGlyphName(double percentage, BatteryState state) {
  if (state == BatteryState::Charging) {
    return "battery-charging";
  }
  if (state == BatteryState::FullyCharged || state == BatteryState::PendingCharge) {
    return "battery-plugged";
  }
  if (state == BatteryState::Unknown && percentage <= 0.0) {
    return "battery-exclamation";
  }
  if (percentage >= 85.0) {
    return "battery-4";
  }
  if (percentage >= 55.0) {
    return "battery-3";
  }
  if (percentage >= 30.0) {
    return "battery-2";
  }
  if (percentage >= 10.0) {
    return "battery-1";
  }
  return "battery-0";
}

const char* batteryDeviceGlyphName(UPowerDeviceType type) {
  switch (type) {
  case UPowerDeviceType::Mouse:
    return "mouse-2";
  case UPowerDeviceType::Keyboard:
    return "keyboard";
  case UPowerDeviceType::Phone:
  case UPowerDeviceType::Pda:
    return "device-mobile";
  default:
    return "bluetooth";
  }
}

namespace {

  template <typename T>
  T getPropertyOr(sdbus::IProxy& proxy, std::string_view iface, std::string_view propertyName, T fallback) {
    try {
      const sdbus::Variant value = proxy.getProperty(propertyName).onInterface(iface);
      return value.get<T>();
    } catch (const sdbus::Error&) {
      return fallback;
    }
  }

  bool isBatteryCapableDeviceType(UPowerDeviceType type) {
    return type != UPowerDeviceType::Unknown && type != UPowerDeviceType::LinePower;
  }

  bool isAutoSelector(std::string_view selector) {
    const std::string normalized = StringUtils::toLower(StringUtils::trim(selector));
    return normalized.empty() || normalized == "auto";
  }

  bool hasSelectorSuffix(std::string_view value, std::string_view selector) {
    if (value.empty() || selector.empty() || value.size() < selector.size()) {
      return false;
    }
    const std::size_t start = value.size() - selector.size();
    if (value.substr(start) != selector) {
      return false;
    }
    if (start == 0) {
      return true;
    }
    const char before = value[start - 1];
    return before == '/' || before == '_' || before == '-' || before == ':' || before == '.';
  }

  bool selectorMatchesField(const std::string& value, std::string_view selector) {
    return std::string_view(value) == selector || hasSelectorSuffix(value, selector);
  }

  BatteryState decodeBatteryState(std::uint32_t raw) {
    if (raw >= 1 && raw <= 6) {
      return static_cast<BatteryState>(raw);
    }
    return BatteryState::Unknown;
  }

  constexpr Logger kLog("upower");

  UPowerDeviceInfo makeDummyBatteryDevice() {
    UPowerDeviceInfo info;
    info.path = "/org/freedesktop/UPower/devices/dummy_battery";
    info.nativePath = "dummy_BAT0";
    info.model = "Dummy Battery";
    info.type = UPowerDeviceType::Battery;
    info.powerSupply = true;
    info.isPresent = true;
    info.energyFull = 54.0;
    info.energyFullDesign = 54.0;
    info.state.percentage = 67.0;
    info.state.energyRate = 12.5;
    info.state.state = BatteryState::Discharging;
    info.state.timeToEmpty = 3 * 3600 + 15 * 60;
    info.state.energy = 36.2;
    info.state.isPresent = true;
    info.state.onBattery = true;
    return info;
  }

} // namespace

bool upowerDeviceMatchesSelector(const UPowerDeviceInfo& info, std::string_view selector) {
  const std::string trimmed = StringUtils::trim(selector);
  if (trimmed.empty()) {
    return false;
  }
  return selectorMatchesField(info.path, trimmed)
      || selectorMatchesField(info.nativePath, trimmed)
      || selectorMatchesField(info.model, trimmed)
      || selectorMatchesField(info.serial, trimmed)
      || selectorMatchesField(info.vendor, trimmed);
}

UPowerService::UPowerService(SystemBus& bus) : m_bus(bus) {
  m_upowerProxy = sdbus::createProxy(m_bus.connection(), kUpowerBusName, kUpowerObjectPath);

  m_upowerProxy->uponSignal("PropertiesChanged")
      .onInterface(kPropertiesInterface)
      .call([this](
                const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& /*changed*/,
                const std::vector<std::string>& /*invalidated*/
            ) {
        if (interfaceName == kUpowerInterface) {
          refresh();
        }
      });

  m_upowerProxy->uponSignal("DeviceAdded").onInterface(kUpowerInterface).call([this](const sdbus::ObjectPath&) {
    rescanDevices();
  });

  m_upowerProxy->uponSignal("DeviceRemoved").onInterface(kUpowerInterface).call([this](const sdbus::ObjectPath&) {
    rescanDevices();
  });

  if (SysUtils::isEnvFlagOn("NOCTALIA_DUMMY_BATTERY")) {
    m_dummyDevice = makeDummyBatteryDevice();
    kLog.info("dummy battery enabled ({:.0f}% discharging)", m_dummyDevice->state.percentage);
  }

  rescanDevices();

  if (m_state.isPresent) {
    kLog.info(
        "battery {:.0f}% state={} ({})", m_state.percentage, static_cast<int>(m_state.state),
        m_state.onBattery ? "on battery" : "on AC"
    );
  } else {
    kLog.info("connected (no system battery present)");
  }
}

void UPowerService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void UPowerService::refresh() { refreshDeviceStates(); }

std::vector<UPowerDeviceInfo> UPowerService::batteryDevices() const {
  std::vector<UPowerDeviceInfo> devices;
  devices.reserve(m_devices.size() + (m_dummyDevice ? 1 : 0));
  for (const auto& device : m_devices) {
    if (device.info.isPresent && isBatteryCapableDeviceType(device.info.type)) {
      devices.push_back(device.info);
    }
  }
  if (m_dummyDevice && m_dummyDevice->isPresent) {
    devices.push_back(*m_dummyDevice);
  }
  return devices;
}

UPowerState UPowerService::stateForDevice(std::string_view selector) const {
  if (isAutoSelector(selector)) {
    return m_state;
  }

  if (const auto* device = deviceForSelector(selector); device != nullptr) {
    return device->state;
  }

  UPowerState missing;
  missing.onBattery = getPropertyOr<bool>(*m_upowerProxy, kUpowerInterface, "OnBattery", false);
  return missing;
}

void UPowerService::rescanDevices() {
  refreshDisplayDeviceProxy();

  std::vector<sdbus::ObjectPath> paths;
  try {
    m_upowerProxy->callMethod("EnumerateDevices").onInterface(kUpowerInterface).storeResultsTo(paths);
  } catch (const sdbus::Error& e) {
    kLog.warn("EnumerateDevices failed: {}", e.what());
    emitChangedIfNeeded(false);
    return;
  }

  std::vector<TrackedDevice> nextDevices;
  nextDevices.reserve(paths.size());
  for (const auto& path : paths) {
    try {
      auto proxy = sdbus::createProxy(m_bus.connection(), kUpowerBusName, path);
      auto info = readDeviceInfo(std::string(path), *proxy);
      if (!isBatteryCapableDeviceType(info.type)) {
        continue;
      }

      proxy->uponSignal("PropertiesChanged")
          .onInterface(kPropertiesInterface)
          .call([this](
                    const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& /*changed*/,
                    const std::vector<std::string>& /*invalidated*/
                ) {
            if (interfaceName == kDeviceInterface) {
              refresh();
            }
          });

      nextDevices.push_back(TrackedDevice{std::move(info), std::move(proxy)});
    } catch (const sdbus::Error&) {
      continue;
    }
  }

  std::ranges::sort(nextDevices, [](const TrackedDevice& lhs, const TrackedDevice& rhs) {
    return lhs.info.path < rhs.info.path;
  });

  bool devicesChanged = m_devices.size() != nextDevices.size();
  if (!devicesChanged) {
    for (std::size_t i = 0; i < m_devices.size(); ++i) {
      if (m_devices[i].info != nextDevices[i].info) {
        devicesChanged = true;
        break;
      }
    }
  }
  m_devices = std::move(nextDevices);
  if (devicesChanged) {
    kLog.debug("tracking {} UPower battery-capable device(s)", m_devices.size());
  }
  emitChangedIfNeeded(devicesChanged);
}

UPowerState UPowerService::readDefaultState() const {
  UPowerState next;

  next.onBattery = getPropertyOr<bool>(*m_upowerProxy, kUpowerInterface, "OnBattery", false);

  if (m_displayDeviceProxy != nullptr) {
    next = readDeviceState(*m_displayDeviceProxy);
    next.onBattery = getPropertyOr<bool>(*m_upowerProxy, kUpowerInterface, "OnBattery", false);
    if (next.isPresent) {
      return next;
    }
  }

  const auto* device = defaultSystemBattery();
  if (device == nullptr) {
    return next;
  }

  next = device->state;
  if (m_dummyDevice && device == &*m_dummyDevice) {
    return next;
  }
  next.onBattery = getPropertyOr<bool>(*m_upowerProxy, kUpowerInterface, "OnBattery", false);
  return next;
}

UPowerState UPowerService::readDeviceState(sdbus::IProxy& proxy) const {
  UPowerState next;

  next.onBattery = getPropertyOr<bool>(*m_upowerProxy, kUpowerInterface, "OnBattery", false);
  next.percentage = getPropertyOr<double>(proxy, kDeviceInterface, "Percentage", 0.0);
  next.isPresent = getPropertyOr<bool>(proxy, kDeviceInterface, "IsPresent", false);
  const auto rawState = getPropertyOr<std::uint32_t>(proxy, kDeviceInterface, "State", 0);
  next.state = decodeBatteryState(rawState);
  next.timeToEmpty = getPropertyOr<std::int64_t>(proxy, kDeviceInterface, "TimeToEmpty", 0);
  next.timeToFull = getPropertyOr<std::int64_t>(proxy, kDeviceInterface, "TimeToFull", 0);
  next.energyRate = getPropertyOr<double>(proxy, kDeviceInterface, "EnergyRate", 0.0);
  next.energy = getPropertyOr<double>(proxy, kDeviceInterface, "Energy", 0.0);

  // Fallback calculation for timeToEmpty / timeToFull if they are reported as 0 or less
  if (next.state == BatteryState::Discharging && next.timeToEmpty <= 0 && next.energyRate > 0.0 && next.energy > 0.0) {
    next.timeToEmpty = static_cast<std::int64_t>(std::round((next.energy / next.energyRate) * 3600.0));
  } else if (next.state == BatteryState::Charging && next.timeToFull <= 0 && next.energyRate > 0.0) {
    const auto energyFull = getPropertyOr<double>(proxy, kDeviceInterface, "EnergyFull", 0.0);
    if (energyFull > next.energy) {
      next.timeToFull = static_cast<std::int64_t>(std::round(((energyFull - next.energy) / next.energyRate) * 3600.0));
    }
  }

  return next;
}

UPowerDeviceInfo UPowerService::readDeviceInfo(std::string path, sdbus::IProxy& proxy) const {
  UPowerDeviceInfo info;
  info.path = std::move(path);
  info.nativePath = getPropertyOr<std::string>(proxy, kDeviceInterface, "NativePath", "");
  info.vendor = getPropertyOr<std::string>(proxy, kDeviceInterface, "Vendor", "");
  info.model = getPropertyOr<std::string>(proxy, kDeviceInterface, "Model", "");
  info.serial = getPropertyOr<std::string>(proxy, kDeviceInterface, "Serial", "");
  info.type = static_cast<UPowerDeviceType>(getPropertyOr<std::uint32_t>(proxy, kDeviceInterface, "Type", 0));
  info.powerSupply = getPropertyOr<bool>(proxy, kDeviceInterface, "PowerSupply", false);
  info.energyFull = getPropertyOr<double>(proxy, kDeviceInterface, "EnergyFull", 0.0);
  info.energyFullDesign = getPropertyOr<double>(proxy, kDeviceInterface, "EnergyFullDesign", 0.0);
  info.state = readDeviceState(proxy);
  info.isPresent = info.state.isPresent;
  return info;
}

const UPowerDeviceInfo* UPowerService::defaultSystemBattery() const noexcept {
  for (const auto& device : m_devices) {
    if (device.info.isLaptopBattery() && device.info.isPresent) {
      return &device.info;
    }
  }
  if (m_dummyDevice && m_dummyDevice->isLaptopBattery() && m_dummyDevice->isPresent) {
    return &*m_dummyDevice;
  }
  return nullptr;
}

const UPowerDeviceInfo* UPowerService::deviceForSelector(std::string_view selector) const {
  const std::string trimmed = StringUtils::trim(selector);
  if (trimmed.empty()) {
    return nullptr;
  }

  for (const auto& device : m_devices) {
    if (isBatteryCapableDeviceType(device.info.type) && upowerDeviceMatchesSelector(device.info, trimmed)) {
      return &device.info;
    }
  }
  if (m_dummyDevice
      && isBatteryCapableDeviceType(m_dummyDevice->type)
      && upowerDeviceMatchesSelector(*m_dummyDevice, trimmed)) {
    return &*m_dummyDevice;
  }
  return nullptr;
}

void UPowerService::refreshDisplayDeviceProxy() {
  sdbus::ObjectPath path;
  try {
    m_upowerProxy->callMethod("GetDisplayDevice").onInterface(kUpowerInterface).storeResultsTo(path);
  } catch (const sdbus::Error& e) {
    kLog.warn("GetDisplayDevice failed: {}", e.what());
    m_displayDeviceProxy.reset();
    m_displayDevicePath.clear();
    return;
  }

  const std::string nextPath(path);
  if (nextPath.empty() || nextPath == "/") {
    m_displayDeviceProxy.reset();
    m_displayDevicePath.clear();
    return;
  }
  if (m_displayDeviceProxy != nullptr && m_displayDevicePath == nextPath) {
    return;
  }

  try {
    auto proxy = sdbus::createProxy(m_bus.connection(), kUpowerBusName, path);
    proxy->uponSignal("PropertiesChanged")
        .onInterface(kPropertiesInterface)
        .call([this](
                  const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& /*changed*/,
                  const std::vector<std::string>& /*invalidated*/
              ) {
          if (interfaceName == kDeviceInterface) {
            refresh();
          }
        });

    m_displayDevicePath = nextPath;
    m_displayDeviceProxy = std::move(proxy);
  } catch (const sdbus::Error& e) {
    kLog.warn("failed to track UPower display device {}: {}", nextPath, e.what());
    m_displayDeviceProxy.reset();
    m_displayDevicePath.clear();
  }
}

void UPowerService::refreshDeviceStates() {
  bool devicesChanged = false;
  for (auto& device : m_devices) {
    auto next = readDeviceInfo(device.info.path, *device.proxy);
    if (next != device.info) {
      device.info = std::move(next);
      devicesChanged = true;
    }
  }
  emitChangedIfNeeded(devicesChanged);
}

void UPowerService::emitChangedIfNeeded(bool devicesChanged) {
  const UPowerState next = readDefaultState();
  if (!devicesChanged && next == m_state) {
    return;
  }

  m_state = next;
  if (m_changeCallback) {
    m_changeCallback();
  }
}
