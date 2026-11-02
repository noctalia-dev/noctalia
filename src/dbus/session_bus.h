#pragma once

#include <memory>
#include <sdbus-c++/sdbus-c++.h>

class SessionBus {
public:
  SessionBus();

  sdbus::IConnection& connection() noexcept { return *m_connection; }
  sdbus::IConnection::PollData getPollData() const;
  bool hasPendingEvents() const noexcept { return m_hasPendingEvents; }
  void processPendingEvents();

private:
  std::unique_ptr<sdbus::IConnection> m_connection;
  bool m_hasPendingEvents = false;
};
