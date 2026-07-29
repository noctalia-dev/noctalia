#pragma once

#include <functional>
#include <memory>
#include <vector>

class IpcService;
class SystemBus;

namespace sdbus {
  class IProxy;
}

class KeyboardBacklightService {
public:
  using ChangeCallback = std::function<void()>;

  explicit KeyboardBacklightService(SystemBus& bus);
  ~KeyboardBacklightService();

  KeyboardBacklightService(const KeyboardBacklightService&) = delete;
  KeyboardBacklightService& operator=(const KeyboardBacklightService&) = delete;

  [[nodiscard]] int brightness() const noexcept { return m_brightness; }
  [[nodiscard]] int maxBrightness() const noexcept { return m_maxBrightness; }
  [[nodiscard]] bool available() const noexcept { return !m_devices.empty(); }

  void registerIpc(IpcService& ipc);
  void setChangeCallback(ChangeCallback callback);

private:
  struct Device;

  void rescanDevices();
  void publishBrightness(const Device& device);
  [[nodiscard]] bool setBrightness(Device& device, int value);
  [[nodiscard]] bool setPercent(int percent);
  [[nodiscard]] bool adjustBrightness(int delta);
  [[nodiscard]] bool toggleBrightness();

  SystemBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_upowerProxy;
  std::vector<std::unique_ptr<Device>> m_devices;
  int m_brightness = 0;
  int m_maxBrightness = 0;
  ChangeCallback m_changeCallback;
};
