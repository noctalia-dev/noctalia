#pragma once

#include <memory>
#include <sdbus-c++/sdbus-c++.h>
#include <string_view>

class SystemBus {
public:
  SystemBus();

  [[nodiscard]] sdbus::IConnection& connection() noexcept { return *m_connection; }
  [[nodiscard]] sdbus::IConnection::PollData getPollData() const;
  [[nodiscard]] bool hasPendingEvents() const noexcept { return m_hasPendingEvents; }
  [[nodiscard]] bool nameHasOwner(std::string_view name) const;
  // Whether systemd starts the unit at boot. False when systemd or the unit is missing.
  [[nodiscard]] bool systemdUnitEnabled(std::string_view unit) const;
  [[nodiscard]] bool isNameOwner(std::string_view name, std::string_view sender) const;
  void processPendingEvents();

private:
  std::unique_ptr<sdbus::IConnection> m_connection;
  bool m_hasPendingEvents = false;
};
