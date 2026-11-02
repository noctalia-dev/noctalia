#pragma once

#include <memory>
#include <sdbus-c++/sdbus-c++.h>
#include <string_view>

class SystemBus {
public:
  SystemBus();

  sdbus::IConnection& connection() noexcept { return *m_connection; }
  sdbus::IConnection::PollData getPollData() const;
  bool hasPendingEvents() const noexcept { return m_hasPendingEvents; }
  bool nameHasOwner(std::string_view name) const;
  // Whether systemd starts the unit at boot. False when systemd or the unit is missing.
  bool systemdUnitEnabled(std::string_view unit) const;
  bool isNameOwner(std::string_view name, std::string_view sender) const;
  void processPendingEvents();

private:
  std::unique_ptr<sdbus::IConnection> m_connection;
  bool m_hasPendingEvents = false;
};
