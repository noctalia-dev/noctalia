#include "dbus/upower/upower_service.h"

#include "core/log.h"
#include "dbus/system_bus.h"
#include "dbus/upower/upower_charge_limit_support.h"
#include "i18n/i18n.h"
#include "util/string_utils.h"
#include "util/sys_utils.h"

#include <algorithm>
#include <array>
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
  constexpr auto kIntrospectableInterface = "org.freedesktop.DBus.Introspectable";

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

  template <typename T>
  std::optional<T> getOptionalProperty(sdbus::IProxy& proxy, std::string_view iface, std::string_view propertyName) {
    try {
      const sdbus::Variant value = proxy.getProperty(propertyName).onInterface(iface);
      return value.get<T>();
    } catch (const sdbus::Error&) {
      return std::nullopt;
    }
  }

  std::optional<std::uint32_t> thresholdProperty(sdbus::IProxy& proxy, std::string_view propertyName) {
    const auto value = getOptionalProperty<std::uint32_t>(proxy, kDeviceInterface, propertyName);
    if (!value.has_value() || *value > 100U) {
      return std::nullopt;
    }
    return value;
  }

  std::optional<bool> hasChargeThresholdMethod(sdbus::IProxy& proxy) {
    try {
      std::string xml;
      proxy.callMethod("Introspect").onInterface(kIntrospectableInterface).storeResultsTo(xml);
      return xml.contains("<method name=\"EnableChargeThreshold\"")
          || xml.contains("<method name='EnableChargeThreshold'");
    } catch (const sdbus::Error&) {
      return std::nullopt;
    }
  }

  bool chargeLimitIsExternallyManaged(const UPowerChargeLimitState& state) {
    return state.enabledAvailable && !state.enabled && state.hasRestrictiveThreshold();
  }

  bool sameDeviceInfoExceptChargeLimit(const UPowerDeviceInfo& lhs, const UPowerDeviceInfo& rhs) {
    auto lhsWithoutChargeLimit = lhs;
    auto rhsWithoutChargeLimit = rhs;
    lhsWithoutChargeLimit.chargeLimit = {};
    rhsWithoutChargeLimit.chargeLimit = {};
    return lhsWithoutChargeLimit == rhsWithoutChargeLimit;
  }

  bool chargeLimitPropertiesChanged(
      const std::map<std::string, sdbus::Variant>& changed, const std::vector<std::string>& invalidated
  ) {
    if (changed.empty() && invalidated.empty()) {
      return true;
    }

    constexpr std::array<std::string_view, 8> properties{
        "NativePath",
        "Type",
        "PowerSupply",
        "ChargeThresholdSupported",
        "ChargeThresholdEnabled",
        "ChargeThresholdSettingsSupported",
        "ChargeStartThreshold",
        "ChargeEndThreshold",
    };
    return std::ranges::any_of(properties, [&changed, &invalidated](std::string_view property) {
      return std::ranges::any_of(changed, [property](const auto& entry) { return entry.first == property; })
          || std::ranges::find(invalidated, property) != invalidated.end();
    });
  }

  bool isAuthorizationError(const sdbus::Error& error) {
    const auto& name = error.getName();
    if (name == sdbus::Error::Name{"org.freedesktop.DBus.Error.AccessDenied"}
        || name == sdbus::Error::Name{"org.freedesktop.PolicyKit1.Error.NotAuthorized"}
        || name == sdbus::Error::Name{"org.freedesktop.UPower.Device.PermissionDenied"}) {
      return true;
    }

    // UPower reports PolicyKit rejection as GeneralError with this canonical message. Its error
    // name is shared with unrelated hardware failures, so only classify recognizable denial text.
    const std::string message = StringUtils::toLower(error.what());
    return message.contains("operation is not allowed")
        || message.contains("not authorized")
        || message.contains("authorization")
        || message.contains("authentication")
        || message.contains("access denied");
  }

  bool isBatteryCapableDeviceType(UPowerDeviceType type) {
    return type != UPowerDeviceType::Unknown && type != UPowerDeviceType::LinePower;
  }

  // A battery belonging to a peripheral rather than to the system. UPower reports peripheral packs
  // with PowerSupply=false, so only PowerSupply batteries and UPS units power the machine itself.
  bool isPeripheralBattery(const UPowerDeviceInfo& info) {
    return info.isPresent
        && isBatteryCapableDeviceType(info.type)
        && !info.isLaptopBattery()
        && info.type != UPowerDeviceType::Ups;
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

std::optional<double> UPowerDeviceInfo::healthPercent() const {
  if (energyFullDesign <= 0.0 || energyFull <= 0.0) {
    return std::nullopt;
  }
  return std::clamp(energyFull / energyFullDesign * 100.0, 0.0, 100.0);
}

bool UPowerChargeLimitState::hasRestrictiveThreshold() const {
  return (effectiveStart.has_value() && *effectiveStart > 0U && *effectiveStart < 100U)
      || (effectiveEnd.has_value() && *effectiveEnd < 100U);
}

bool UPowerDeviceInfo::sameCatalogEntry(const UPowerDeviceInfo& other) const {
  return path == other.path
      && nativePath == other.nativePath
      && vendor == other.vendor
      && model == other.model
      && serial == other.serial
      && type == other.type
      && powerSupply == other.powerSupply
      && isPresent == other.isPresent;
}

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
          (void)refreshDefaultState();
        }
      });

  m_upowerProxy->uponSignal("DeviceAdded").onInterface(kUpowerInterface).call([this](const sdbus::ObjectPath&) {
    rescanDevices();
  });

  m_upowerProxy->uponSignal("DeviceRemoved").onInterface(kUpowerInterface).call([this](const sdbus::ObjectPath& path) {
    invalidateChargeThresholdRequest(std::string(path));
    rescanDevices();
  });

  if (SysUtils::isEnvFlagOn("NOCTALIA_DUMMY_BATTERY")) {
    m_dummyDevice = makeDummyBatteryDevice();
    kLog.info("dummy battery enabled ({:.0F}% discharging)", m_dummyDevice->state.percentage);
  }

  rescanDevices();

  if (m_state.isPresent) {
    kLog.info(
        "battery {:.0F}% state={} ({})", m_state.percentage, static_cast<int>(m_state.state),
        m_state.onBattery ? "on battery" : "on AC"
    );
  } else {
    kLog.info("connected (no system battery present)");
  }
}

UPowerService::~UPowerService() { m_lifetimeToken.reset(); }

void UPowerService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void UPowerService::refresh() {
  const auto changes = refreshDeviceStates();
  if (changes.devicesChanged) {
    emitChangedIfNeeded(true, changes.chargeLimitChanged, changes.deviceCatalogChanged);
  } else if (!refreshDefaultState()) {
    emitChangedIfNeeded(false, changes.chargeLimitChanged, changes.deviceCatalogChanged);
  }
}

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
    (void)refreshDefaultState();
    return;
  }

  std::vector<TrackedDevice> nextDevices;
  nextDevices.reserve(paths.size());
  for (const auto& path : paths) {
    try {
      const std::string devicePath(path);
      const auto previous = std::ranges::find_if(m_devices, [&devicePath](const TrackedDevice& device) {
        return device.info.path == devicePath;
      });
      std::optional<bool> knownMethod =
          previous != m_devices.end() ? previous->chargeThresholdMethodAvailable : std::nullopt;
      auto proxy = sdbus::createProxy(m_bus.connection(), kUpowerBusName, path);
      auto info = readDeviceInfo(devicePath, *proxy, knownMethod);
      if (!isBatteryCapableDeviceType(info.type)) {
        continue;
      }

      const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
      proxy->uponSignal("PropertiesChanged")
          .onInterface(kPropertiesInterface)
          .call([this, lifetimeToken, devicePath](
                    const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& changed,
                    const std::vector<std::string>& invalidated
                ) {
            if (!lifetimeToken.expired() && interfaceName == kDeviceInterface) {
              const auto changes = refreshDevice(devicePath, chargeLimitPropertiesChanged(changed, invalidated));
              emitChangedIfNeeded(changes.devicesChanged, changes.chargeLimitChanged, changes.deviceCatalogChanged);
            }
          });

      std::optional<std::uint64_t> requestId;
      if (previous != m_devices.end()) {
        info.chargeLimit.requestPending = previous->info.chargeLimit.requestPending;
        info.chargeLimit.requestedEnabled = previous->info.chargeLimit.requestedEnabled;
        info.chargeLimit.operationError = previous->info.chargeLimit.operationError;
        requestId = previous->chargeThresholdRequestId;
      }
      nextDevices.push_back(
          TrackedDevice{std::move(info), std::shared_ptr<sdbus::IProxy>(std::move(proxy)), knownMethod, requestId}
      );
    } catch (const sdbus::Error&) {
      continue;
    }
  }

  std::ranges::sort(nextDevices, [](const TrackedDevice& lhs, const TrackedDevice& rhs) {
    return lhs.info.path < rhs.info.path;
  });

  bool devicesChanged = m_devices.size() != nextDevices.size();
  bool chargeLimitChanged = false;
  bool deviceCatalogChanged = devicesChanged;
  if (!devicesChanged) {
    for (std::size_t i = 0; i < m_devices.size(); ++i) {
      if (!m_devices[i].info.sameCatalogEntry(nextDevices[i].info)) {
        deviceCatalogChanged = true;
      }
      if (!sameDeviceInfoExceptChargeLimit(m_devices[i].info, nextDevices[i].info)) {
        devicesChanged = true;
      }
      chargeLimitChanged = chargeLimitChanged || m_devices[i].info.chargeLimit != nextDevices[i].info.chargeLimit;
    }
  }
  m_devices = std::move(nextDevices);
  if (deviceCatalogChanged) {
    kLog.debug("tracking {} UPower battery-capable device(s)", m_devices.size());
  }
  if (devicesChanged) {
    emitChangedIfNeeded(true, chargeLimitChanged, deviceCatalogChanged);
  } else if (!refreshDefaultState()) {
    emitChangedIfNeeded(false, chargeLimitChanged, deviceCatalogChanged);
  }
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

UPowerDeviceInfo UPowerService::readDeviceInfoBase(std::string path, sdbus::IProxy& proxy) const {
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

UPowerChargeLimitState UPowerService::readChargeLimitState(
    const UPowerDeviceInfo& info, sdbus::IProxy& proxy, std::optional<bool>& chargeThresholdMethodAvailable
) const {
  UPowerChargeLimitState state;
  if (!info.isLaptopBattery()) {
    chargeThresholdMethodAvailable.reset();
    return state;
  }

  state.supported = getOptionalProperty<bool>(proxy, kDeviceInterface, "ChargeThresholdSupported").value_or(false);
  if (state.supported && !chargeThresholdMethodAvailable.has_value()) {
    const auto detectedAvailability = hasChargeThresholdMethod(proxy);
    if (detectedAvailability.has_value()) {
      chargeThresholdMethodAvailable = detectedAvailability;
    }
  }
  state.methodAvailable = state.supported && chargeThresholdMethodAvailable.value_or(false);
  const auto enabled = getOptionalProperty<bool>(proxy, kDeviceInterface, "ChargeThresholdEnabled");
  state.enabledAvailable = enabled.has_value();
  state.enabled = enabled.value_or(false);
  state.supportedSettings =
      getOptionalProperty<std::uint32_t>(proxy, kDeviceInterface, "ChargeThresholdSettingsSupported");
  if (!state.supportedSettings.has_value() || (*state.supportedSettings & 1U) != 0U) {
    state.configuredStart = thresholdProperty(proxy, "ChargeStartThreshold");
  }
  if (!state.supportedSettings.has_value() || (*state.supportedSettings & 2U) != 0U) {
    state.configuredEnd = thresholdProperty(proxy, "ChargeEndThreshold");
  }
  const auto effective = upower::detail::readChargeThresholdsFromSysfs(info.nativePath);
  state.effectiveStart = effective.start;
  state.effectiveEnd = effective.end;
  return state;
}

UPowerDeviceInfo UPowerService::readDeviceInfo(
    std::string path, sdbus::IProxy& proxy, std::optional<bool>& chargeThresholdMethodAvailable
) const {
  auto info = readDeviceInfoBase(std::move(path), proxy);
  info.chargeLimit = readChargeLimitState(info, proxy, chargeThresholdMethodAvailable);
  return info;
}

bool UPowerService::enableChargeThreshold(std::string_view devicePath, bool enabled) {
  const auto it = std::ranges::find_if(m_devices, [devicePath](const TrackedDevice& device) {
    return device.info.path == devicePath;
  });
  if (it == m_devices.end()
      || !it->info.isLaptopBattery()
      || !it->info.chargeLimit.supported
      || !it->info.chargeLimit.methodAvailable
      || !it->info.chargeLimit.enabledAvailable
      || chargeLimitIsExternallyManaged(it->info.chargeLimit)
      || it->info.chargeLimit.requestPending) {
    return false;
  }

  const std::string path = it->info.path;
  const auto proxy = it->proxy;
  const std::uint64_t requestId = m_nextChargeThresholdRequestId++;
  it->chargeThresholdRequestId = requestId;
  auto& operation = it->info.chargeLimit;
  operation.requestPending = true;
  operation.requestedEnabled = enabled;
  operation.operationError = ChargeLimitOperationError::None;
  emitControlOperationChanged();

  const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
  try {
    proxy->callMethodAsync("EnableChargeThreshold")
        .onInterface(kDeviceInterface)
        .withArguments(enabled)
        .uponReplyInvoke([this, lifetimeToken, path, requestId, keepAlive = proxy](std::optional<sdbus::Error> error) {
          (void)keepAlive;
          if (lifetimeToken.expired()) {
            return;
          }

          const auto current = std::ranges::find_if(m_devices, [&path](const TrackedDevice& device) {
            return device.info.path == path;
          });
          if (current == m_devices.end() || current->chargeThresholdRequestId != requestId) {
            return;
          }

          // Refresh on every completion: UPower may have changed a subset of the hardware state even
          // when the method reports an error.
          auto changes = refreshDeviceStates();
          const auto refreshed = std::ranges::find_if(m_devices, [&path](const TrackedDevice& device) {
            return device.info.path == path;
          });
          if (refreshed != m_devices.end() && refreshed->chargeThresholdRequestId == requestId) {
            refreshed->chargeThresholdRequestId.reset();
            refreshed->info.chargeLimit.requestPending = false;
            refreshed->info.chargeLimit.requestedEnabled.reset();
            refreshed->info.chargeLimit.operationError = !error.has_value()
                ? ChargeLimitOperationError::None
                : (isAuthorizationError(*error) ? ChargeLimitOperationError::PermissionDenied
                                                : ChargeLimitOperationError::Failed);
            if (error.has_value()) {
              kLog.warn("charge threshold change failed device={} err={}", path, error->what());
            }
            changes.chargeLimitChanged = true;
            emitChangedIfNeeded(changes.devicesChanged, changes.chargeLimitChanged, changes.deviceCatalogChanged);
          }
        });
  } catch (const sdbus::Error& error) {
    const auto current =
        std::ranges::find_if(m_devices, [&path](const TrackedDevice& device) { return device.info.path == path; });
    if (current != m_devices.end() && current->chargeThresholdRequestId == requestId) {
      current->chargeThresholdRequestId.reset();
      current->info.chargeLimit.requestPending = false;
      current->info.chargeLimit.requestedEnabled.reset();
      current->info.chargeLimit.operationError = ChargeLimitOperationError::Failed;
    }
    kLog.warn("charge threshold change dispatch failed device={} err={}", path, error.what());
    emitControlOperationChanged();
    return false;
  }
  return true;
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

const UPowerDeviceInfo* UPowerService::peripheralBatteryForSerial(std::string_view serial) const {
  if (serial.empty()) {
    return nullptr;
  }
  for (const auto& device : m_devices) {
    if (isPeripheralBattery(device.info) && StringUtils::equalsInsensitive(device.info.serial, serial)) {
      return &device.info;
    }
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
            (void)refreshDefaultState();
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

bool UPowerService::refreshDefaultState() {
  const UPowerState next = readDefaultState();
  if (next == m_state) {
    return false;
  }

  m_state = next;
  if (m_changeCallback) {
    m_changeCallback(UPowerChange{.origin = ChangeOrigin::DeviceState});
  }
  return true;
}

UPowerService::RefreshChanges UPowerService::refreshTrackedDevice(TrackedDevice& device, bool refreshChargeLimit) {
  RefreshChanges changes;
  auto next = readDeviceInfoBase(device.info.path, *device.proxy);
  const bool laptopCapabilityChanged = next.isLaptopBattery() != device.info.isLaptopBattery();
  if (refreshChargeLimit || laptopCapabilityChanged) {
    next.chargeLimit = readChargeLimitState(next, *device.proxy, device.chargeThresholdMethodAvailable);
    next.chargeLimit.requestPending = device.info.chargeLimit.requestPending;
    next.chargeLimit.requestedEnabled = device.info.chargeLimit.requestedEnabled;
    next.chargeLimit.operationError = device.info.chargeLimit.operationError;
  } else {
    next.chargeLimit = device.info.chargeLimit;
  }

  changes.devicesChanged = !sameDeviceInfoExceptChargeLimit(next, device.info);
  changes.chargeLimitChanged = next.chargeLimit != device.info.chargeLimit;
  changes.deviceCatalogChanged = !device.info.sameCatalogEntry(next);
  if (next != device.info) {
    device.info = std::move(next);
  }
  return changes;
}

UPowerService::RefreshChanges UPowerService::refreshDevice(std::string_view devicePath, bool refreshChargeLimit) {
  const auto device = std::ranges::find_if(m_devices, [devicePath](const TrackedDevice& candidate) {
    return candidate.info.path == devicePath;
  });
  if (device == m_devices.end()) {
    return {};
  }
  return refreshTrackedDevice(*device, refreshChargeLimit);
}

UPowerService::RefreshChanges UPowerService::refreshDeviceStates() {
  RefreshChanges changes;
  for (auto& device : m_devices) {
    const auto deviceChanges = refreshTrackedDevice(device, true);
    changes.devicesChanged = changes.devicesChanged || deviceChanges.devicesChanged;
    changes.chargeLimitChanged = changes.chargeLimitChanged || deviceChanges.chargeLimitChanged;
    changes.deviceCatalogChanged = changes.deviceCatalogChanged || deviceChanges.deviceCatalogChanged;
  }
  return changes;
}

void UPowerService::emitChangedIfNeeded(bool devicesChanged, bool chargeLimitChanged, bool deviceCatalogChanged) {
  if (!devicesChanged) {
    if (chargeLimitChanged && m_changeCallback) {
      m_changeCallback(UPowerChange{.origin = ChangeOrigin::ChargeLimit});
    }
    return;
  }

  m_state = readDefaultState();
  if (m_changeCallback) {
    m_changeCallback(UPowerChange{.origin = ChangeOrigin::DeviceState, .deviceCatalogChanged = deviceCatalogChanged});
  }
}

void UPowerService::emitControlOperationChanged() {
  if (m_changeCallback) {
    m_changeCallback(UPowerChange{.origin = ChangeOrigin::ChargeLimit});
  }
}

void UPowerService::invalidateChargeThresholdRequest(std::string_view devicePath) {
  const auto device = std::ranges::find_if(m_devices, [devicePath](const TrackedDevice& candidate) {
    return candidate.info.path == devicePath;
  });
  if (device == m_devices.end()) {
    return;
  }
  device->chargeThresholdRequestId.reset();
  device->info.chargeLimit.requestPending = false;
  device->info.chargeLimit.requestedEnabled.reset();
}
