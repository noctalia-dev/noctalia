#pragma once

#include "core/timer_manager.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class SystemBus;
class IpcService;

namespace sdbus {
  class IProxy;
}

enum class BluetoothDeviceKind : std::uint8_t {
  Unknown,
  Headset,
  Headphones,
  Earbuds,
  Speaker,
  Microphone,
  Mouse,
  Keyboard,
  Phone,
  Computer,
  Gamepad,
  Watch,
  Tv,
};

struct BluetoothDeviceInfo {
  std::string path;
  std::string address;
  std::string alias;
  BluetoothDeviceKind kind = BluetoothDeviceKind::Unknown;
  bool paired = false;
  bool trusted = false;
  bool connected = false;
  bool connecting = false;
  bool hasRssi = false;
  std::int16_t rssi = 0;
  bool hasBattery = false;
  std::uint8_t batteryPercent = 0;

  bool operator==(const BluetoothDeviceInfo&) const = default;
};

struct BluetoothState {
  bool adapterPresent = false;
  bool powered = false;
  /// rfkill soft block (e.g. GDM/GNOME login screen) prevents powering on until cleared.
  bool rfkillSoftBlocked = false;
  bool rfkillHardBlocked = false;
  bool discoverable = false;
  bool pairable = false;
  bool discovering = false;
  std::string adapterName;

  bool operator==(const BluetoothState&) const = default;
};

enum class BluetoothStateChangeOrigin : std::uint8_t {
  External,
  Noctalia,
};

class BluetoothService {
public:
  using StateCallback = std::function<void(const BluetoothState&, BluetoothStateChangeOrigin)>;
  using DevicesCallback = std::function<void(const std::vector<BluetoothDeviceInfo>&)>;
  using StateFeedbackCallback = std::function<void(bool enabled)>;

  explicit BluetoothService(SystemBus& bus);
  ~BluetoothService();

  BluetoothService(const BluetoothService&) = delete;
  BluetoothService& operator=(const BluetoothService&) = delete;

  void setStateCallback(StateCallback callback);
  void setDevicesCallback(DevicesCallback callback);
  void refresh();
  void registerIpc(IpcService& ipc, StateFeedbackCallback stateFeedback = {});

  [[nodiscard]] const BluetoothState& state() const noexcept { return m_state; }
  [[nodiscard]] bool hasStateSnapshot() const noexcept { return m_hasStateSnapshot; }
  [[nodiscard]] const std::vector<BluetoothDeviceInfo>& devices() const noexcept { return m_devices; }

  void setPowered(bool enabled);
  void setDiscoverable(bool enabled);
  void setPairable(bool enabled);
  void startDiscovery();
  void stopDiscovery();

  bool connect(const std::string& devicePath);
  bool disconnectDevice(const std::string& devicePath);
  bool pair(const std::string& devicePath);
  bool cancelPair(const std::string& devicePath);
  void setTrusted(const std::string& devicePath, bool trusted);
  void forget(const std::string& devicePath);

private:
  struct Impl;
  friend struct Impl;

  BluetoothDeviceInfo* findDevice(const std::string& path);
  [[nodiscard]] BluetoothStateChangeOrigin consumePoweredChangeOrigin(bool powered);
  void emitState(BluetoothStateChangeOrigin origin = BluetoothStateChangeOrigin::External);
  void emitDevices();

  /// Actively reconnect trusted+paired devices after the adapter powers on or at startup: BlueZ
  /// keeps them Trusted but does not dial out to them, so many devices (audio especially) stay
  /// disconnected until the host calls Connect(). Retries a few times with backoff to let the
  /// controller and devices settle.
  void scheduleAutoReconnect();
  void armAutoReconnect();
  void runAutoReconnectPass();

  std::unique_ptr<Impl> m_impl;

  BluetoothState m_state;
  std::vector<BluetoothDeviceInfo> m_devices;
  std::optional<bool> m_pendingLocalPowered;
  Timer m_autoReconnectTimer;
  int m_autoReconnectAttempt = 0;
  bool m_hasStateSnapshot = false;
  StateCallback m_stateCallback;
  DevicesCallback m_devicesCallback;
};
