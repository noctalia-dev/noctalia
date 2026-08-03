#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class SystemBus;

// MMModemState from the ModemManager D-Bus API (org.freedesktop.ModemManager1.Modem "State").
enum class CellularModemState : std::int8_t {
  Failed = -1,
  Unknown = 0,
  Initializing = 1,
  Locked = 2,
  Disabled = 3,
  Disabling = 4,
  Enabling = 5,
  Enabled = 6,
  Searching = 7,
  Registered = 8,
  Disconnecting = 9,
  Connecting = 10,
  Connected = 11,
};

struct CellularModemInfo {
  std::string path;         // ModemManager modem object path.
  std::string name;         // Manufacturer and model, e.g. "Quectel EG25-G"; may be empty.
  std::string operatorName; // Registered network operator; empty while unregistered.
  CellularModemState state = CellularModemState::Unknown;
  std::uint32_t accessTechnologies = 0; // MMModemAccessTechnology bitmask.
  std::uint8_t signalQuality = 0;       // 0..100

  bool operator==(const CellularModemInfo&) const = default;

  // Enabling counts as enabled so a toggle does not flicker off during the transition.
  [[nodiscard]] bool enabled() const noexcept {
    return static_cast<std::int8_t>(state) >= static_cast<std::int8_t>(CellularModemState::Enabling);
  }
  [[nodiscard]] bool connected() const noexcept { return state == CellularModemState::Connected; }
};

// Fastest generation present in an MMModemAccessTechnology bitmask ("5G", "LTE", ...).
// Technical names, intentionally not translated. Empty when nothing useful is reported.
[[nodiscard]] const char* cellularAccessTechnologyName(std::uint32_t accessTechnologies) noexcept;

// Translated, human-readable form of a modem state ("Connected", "Searching…", ...).
[[nodiscard]] std::string cellularStateText(CellularModemState state);

class ModemManagerService {
public:
  using ChangeCallback = std::function<void()>;

  explicit ModemManagerService(SystemBus& bus);
  ~ModemManagerService();

  ModemManagerService(const ModemManagerService&) = delete;
  ModemManagerService& operator=(const ModemManagerService&) = delete;

  void setChangeCallback(ChangeCallback callback);
  void refresh();

  [[nodiscard]] const std::vector<CellularModemInfo>& modems() const noexcept { return m_modems; }
  [[nodiscard]] bool hasStateSnapshot() const noexcept { return m_hasStateSnapshot; }

  // The modem that best represents the device right now: a connected one if any,
  // otherwise the first enabled one, otherwise the first modem at all.
  [[nodiscard]] const CellularModemInfo* primaryModem() const noexcept;

  // Enable / disable the modem radio (Modem.Enable). State arrives via PropertiesChanged.
  void setModemEnabled(const std::string& modemPath, bool enabled);
  void setAllModemsEnabled(bool enabled);

private:
  struct Impl;
  friend struct Impl;

  void emitChanged();

  std::unique_ptr<Impl> m_impl;
  std::vector<CellularModemInfo> m_modems;
  bool m_hasStateSnapshot = false;
  ChangeCallback m_changeCallback;
};
