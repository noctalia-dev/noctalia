#include "system/keyboard_backlight_service.h"

#include "core/log.h"
#include "dbus/system_bus.h"
#include "ipc/ipc_arg_parse.h"
#include "ipc/ipc_service.h"
#include "util/clamp.h"
#include "util/string_utils.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("keyboard-backlight");

  const sdbus::ServiceName kUpowerBusName{"org.freedesktop.UPower"};
  const sdbus::ObjectPath kUpowerPath{"/org/freedesktop/UPower"};
  constexpr auto kUpowerInterface = "org.freedesktop.UPower";
  constexpr auto kKbdBacklightInterface = "org.freedesktop.UPower.KbdBacklight";
  constexpr std::string_view kKbdBacklightPathPrefix = "/org/freedesktop/UPower/KbdBacklight/";

  bool isKbdBacklightPath(const sdbus::ObjectPath& path) {
    return std::string_view{path}.starts_with(kKbdBacklightPathPrefix);
  }

  std::optional<std::string> rejectArgs(std::string_view command, const std::string& args) {
    if (StringUtils::trim(args).empty()) {
      return std::nullopt;
    }
    return "error: " + std::string(command) + " takes no arguments\n";
  }

  std::optional<int> parseInt(const std::string& s) {
    int value = 0;
    const auto result = std::from_chars(s.data(), s.data() + s.size(), value);
    if (result.ec == std::errc() && result.ptr == s.data() + s.size()) {
      return value;
    }
    return std::nullopt;
  }

} // namespace

struct KeyboardBacklightService::Device {
  std::string path;
  std::unique_ptr<sdbus::IProxy> proxy;
  int brightness = 0;
  int maxBrightness = 0;
};

KeyboardBacklightService::KeyboardBacklightService(SystemBus& bus) : m_bus(bus) {
  m_upowerProxy = sdbus::createProxy(m_bus.connection(), kUpowerBusName, kUpowerPath);
  m_upowerProxy->uponSignal("DeviceAdded").onInterface(kUpowerInterface).call([this](const sdbus::ObjectPath& path) {
    if (isKbdBacklightPath(path)) {
      rescanDevices();
    }
  });
  m_upowerProxy->uponSignal("DeviceRemoved").onInterface(kUpowerInterface).call([this](const sdbus::ObjectPath& path) {
    if (isKbdBacklightPath(path)) {
      rescanDevices();
    }
  });
  rescanDevices();
}

KeyboardBacklightService::~KeyboardBacklightService() = default;

void KeyboardBacklightService::rescanDevices() {
  std::vector<sdbus::ObjectPath> paths;
  try {
    m_upowerProxy->callMethod("EnumerateKbdBacklights").onInterface(kUpowerInterface).storeResultsTo(paths);
  } catch (const sdbus::Error& e) {
    kLog.warn("failed to enumerate keyboard backlights: {}", e.what());
    return;
  }

  std::vector<std::unique_ptr<Device>> devices;
  devices.reserve(paths.size());
  for (const auto& path : paths) {
    try {
      auto device = std::make_unique<Device>();
      device->path = path;
      device->proxy = sdbus::createProxy(m_bus.connection(), kUpowerBusName, path);
      device->proxy->callMethod("GetMaxBrightness")
          .onInterface(kKbdBacklightInterface)
          .storeResultsTo(device->maxBrightness);
      device->proxy->callMethod("GetBrightness").onInterface(kKbdBacklightInterface).storeResultsTo(device->brightness);
      if (device->maxBrightness < 0) {
        kLog.warn("keyboard backlight {} reported invalid maximum {}", device->path, device->maxBrightness);
        continue;
      }
      device->brightness = util::clampOrdered(device->brightness, 0, device->maxBrightness);

      const std::string devicePath = device->path;
      device->proxy->uponSignal("BrightnessChanged")
          .onInterface(kKbdBacklightInterface)
          .call([this, devicePath](int32_t value) {
            const auto it = std::ranges::find_if(m_devices, [&devicePath](const auto& candidate) {
              return candidate->path == devicePath;
            });
            if (it == m_devices.end()) {
              return;
            }
            auto& changed = **it;
            const int brightness = util::clampOrdered<int>(value, 0, changed.maxBrightness);
            if (brightness == changed.brightness) {
              return;
            }
            changed.brightness = brightness;
            publishBrightness(changed);
          });
      devices.push_back(std::move(device));
    } catch (const sdbus::Error& e) {
      kLog.warn("failed to initialize keyboard backlight {}: {}", std::string(path), e.what());
    }
  }

  m_devices = std::move(devices);
  if (m_devices.empty()) {
    m_brightness = 0;
    m_maxBrightness = 0;
    kLog.info("no keyboard backlights available");
    return;
  }

  m_brightness = m_devices.front()->brightness;
  m_maxBrightness = m_devices.front()->maxBrightness;
  kLog.info("keyboard backlight service active ({} device(s))", m_devices.size());
}

void KeyboardBacklightService::publishBrightness(const Device& device) {
  m_brightness = device.brightness;
  m_maxBrightness = device.maxBrightness;
  if (m_changeCallback) {
    m_changeCallback();
  }
}

bool KeyboardBacklightService::setBrightness(Device& device, int value) {
  const int clamped = util::clampOrdered(value, 0, device.maxBrightness);
  try {
    device.proxy->callMethod("SetBrightness").onInterface(kKbdBacklightInterface).withArguments(clamped);
  } catch (const sdbus::Error& e) {
    kLog.warn("failed to set keyboard backlight {}: {}", device.path, e.what());
    return false;
  }
  if (device.brightness != clamped) {
    device.brightness = clamped;
    publishBrightness(device);
  }
  return true;
}

bool KeyboardBacklightService::setPercent(int percent) {
  bool success = true;
  for (auto& device : m_devices) {
    const int raw = static_cast<int>(std::round(percent / 100.0 * device->maxBrightness));
    if (!setBrightness(*device, raw)) {
      success = false;
    }
  }
  return success;
}

bool KeyboardBacklightService::adjustBrightness(int delta) {
  bool success = true;
  for (auto& device : m_devices) {
    if (!setBrightness(*device, device->brightness + delta)) {
      success = false;
    }
  }
  return success;
}

bool KeyboardBacklightService::toggleBrightness() {
  bool success = true;
  for (auto& device : m_devices) {
    if (!setBrightness(*device, device->brightness > 0 ? 0 : device->maxBrightness)) {
      success = false;
    }
  }
  return success;
}

void KeyboardBacklightService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void KeyboardBacklightService::registerIpc(IpcService& ipc) {
  ipc.registerHandler(
      "keyboard-backlight-set",
      [this](const std::string& args) -> std::string {
        const auto parts = noctalia::ipc::splitWords(args);
        if (parts.size() != 1) {
          return "error: keyboard-backlight-set requires <value>\n";
        }
        const auto parsed = parseInt(parts[0]);
        if (!parsed.has_value() || *parsed < 0 || *parsed > 100) {
          return "error: invalid keyboard backlight value (use 0-100 for percentage)\n";
        }
        if (!available()) {
          return "error: keyboard backlight unavailable\n";
        }
        return setPercent(*parsed) ? "ok\n" : "error: failed to set keyboard backlight\n";
      },
      "keyboard-backlight-set <value>", "Set all keyboard backlights (0-100 percentage)"
  );

  ipc.registerHandler(
      "keyboard-backlight-up",
      [this](const std::string& args) -> std::string {
        if (auto err = rejectArgs("keyboard-backlight-up", args); err.has_value()) {
          return *err;
        }
        if (!available()) {
          return "error: keyboard backlight unavailable\n";
        }
        return adjustBrightness(1) ? "ok\n" : "error: failed to set keyboard backlight\n";
      },
      "keyboard-backlight-up", "Increase all keyboard backlights by one level"
  );

  ipc.registerHandler(
      "keyboard-backlight-down",
      [this](const std::string& args) -> std::string {
        if (auto err = rejectArgs("keyboard-backlight-down", args); err.has_value()) {
          return *err;
        }
        if (!available()) {
          return "error: keyboard backlight unavailable\n";
        }
        return adjustBrightness(-1) ? "ok\n" : "error: failed to set keyboard backlight\n";
      },
      "keyboard-backlight-down", "Decrease all keyboard backlights by one level"
  );

  ipc.registerHandler(
      "keyboard-backlight-toggle",
      [this](const std::string& args) -> std::string {
        if (auto err = rejectArgs("keyboard-backlight-toggle", args); err.has_value()) {
          return *err;
        }
        if (!available()) {
          return "error: keyboard backlight unavailable\n";
        }
        return toggleBrightness() ? "ok\n" : "error: failed to set keyboard backlight\n";
      },
      "keyboard-backlight-toggle", "Toggle all keyboard backlights on/off"
  );
}
